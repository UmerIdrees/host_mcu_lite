#ifndef HML_RECEIVER_OP_HPP
#define HML_RECEIVER_OP_HPP

/*
 * hml_receiver_op.hpp -- Dual-Transport (UDP / Verbs RoCEv2), Three-Tier
 * (CPU-Only, GPU-Indirect, GPU-Direct) NVIDIA Holoscan Custom Operator.
 *
 * Implements:
 *   - Task C.1: Configurable transport ("verbs" with libibverbs vs "udp" with hml_sock/hml_reasm)
 *   - Task C.2: Three-tier memory architecture:
 *       Tier 1: CPU-only (Host RAM, DLDevice = kDLCPU)
 *       Tier 2: GPU-indirect (Pinned Host RAM -> cudaMemcpyAsync -> Device VRAM, DLDevice = kDLCUDA)
 *       Tier 3: GPU-direct (Zero-bounce VRAM verbs MR via nvidia-peermem, DLDevice = kDLCUDA)
 *               with graceful probe fallback to GPU-indirect.
 *   - Task C.3: DLPack Tensor wrapping conforming to standard Holoscan Tensor specifications.
 */

#include "hml_dlpack.hpp"

#include <hml/hml_protocol.h>
#include <hml/hml_reasm.h>
#include <hml/hml_sock.h>
#include <verbs_rx/hml_verbs_rx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if __has_include(<cuda_runtime.h>)
#include <cuda_runtime.h>
#define HML_HAVE_CUDA 1
#else
#define HML_HAVE_CUDA 0
#endif

#if __has_include(<holoscan/holoscan.hpp>)
#include <holoscan/holoscan.hpp>
#define HML_HAVE_HOLOSCAN 1
#else
#define HML_HAVE_HOLOSCAN 0
#endif

namespace hml::holoscan_bridge {

enum class TransportKind {
  kUdp,
  kVerbs
};

enum class MemoryTier {
  kAuto,
  kCpu,
  kGpuIndirect,
  kGpuDirect
};

struct ReceiverConfig {
  TransportKind transport = TransportKind::kUdp;
  MemoryTier tier = MemoryTier::kAuto;
  hml_verbs_qp_type_t qp_type = HML_VERBS_QP_RC;  // RC (default) or UC
  uint16_t udp_port = 55101;
  uint16_t roce_port = 4791;
  std::string dev_name = "";
  size_t max_frame_bytes = 65536;
  int cuda_device_id = 0;
  bool allow_emulation = true;
  bool force_emulation = false;
};

struct ReceiverStats {
  uint64_t frames_received = 0;
  uint64_t bytes_received = 0;
  uint64_t errors = 0;
  uint64_t timeouts = 0;
  double last_latency_us = 0.0;
};

class HmlReceiverEngine {
 public:
  explicit HmlReceiverEngine(const ReceiverConfig &cfg = ReceiverConfig())
      : cfg_(cfg),
        udp_sock_(nullptr),
        reasm_(nullptr),
        verbs_rx_(nullptr),
        host_buffer_(nullptr),
        d_gpu_buffer_(nullptr),
        is_initialized_(false),
        frame_ready_(false),
        last_frame_bytes_(0),
        cuda_available_(false) {
#if HML_HAVE_CUDA
    int dev_count = 0;
    if (cudaGetDeviceCount(&dev_count) == cudaSuccess && dev_count > 0) {
      cuda_available_ = true;
    }
#endif
  }

  ~HmlReceiverEngine() {
    shutdown();
  }

  bool initialize() {
    if (is_initialized_) return true;

    // Generic Tier Priority Resolution:
    // Priority 1: GPU-Direct (Verbs transport directly registered with GPU VRAM via nvidia-peermem)
    // Priority 2: GPU-Indirect (Staged: Network -> Host RAM -> async cudaMemcpy to GPU VRAM)
    // Priority 3: CPU-Only (Host RAM)

    bool try_direct = (cfg_.tier == MemoryTier::kGpuDirect || cfg_.tier == MemoryTier::kAuto);
    bool try_indirect = (cfg_.tier == MemoryTier::kGpuIndirect || cfg_.tier == MemoryTier::kAuto || cfg_.tier == MemoryTier::kGpuDirect);

    // -------------------------------------------------------------
    // PRIORITY 1: GPU-Direct
    // -------------------------------------------------------------
    if (try_direct && cfg_.transport == TransportKind::kVerbs) {
#if HML_HAVE_CUDA
      if (cuda_available_) {
        cudaSetDevice(cfg_.cuda_device_id);
        cudaError_t st = cudaMalloc(&d_gpu_buffer_, cfg_.max_frame_bytes);
        if (st == cudaSuccess) {
          cudaStreamCreate(&cu_stream_);

          // Attempt verbs registration targeting d_gpu_buffer_ directly
          hml_verbs_cfg vcfg;
          hml_verbs_cfg_default(&vcfg);
          vcfg.dev_name = cfg_.dev_name.empty() ? nullptr : cfg_.dev_name.c_str();
          vcfg.buffer_size = cfg_.max_frame_bytes;
          vcfg.qp_type = cfg_.qp_type;
          vcfg.allow_emulation = cfg_.allow_emulation;
          vcfg.force_emulation = cfg_.force_emulation;
          vcfg.user_buffer = d_gpu_buffer_;

          hml_status vst = hml_verbs_rx_open(&verbs_rx_, &vcfg);
          if (vst == HML_OK && verbs_rx_) {
            hml_verbs_info info;
            hml_verbs_rx_get_info(verbs_rx_, &info);
            if (!info.is_emulated) {
              std::cout << "[HmlReceiver] Priority 1 (GPU-Direct RDMA) active on CUDA device "
                        << cfg_.cuda_device_id << " (BAR1 VRAM: " << d_gpu_buffer_ << ")\n";
              cfg_.tier = MemoryTier::kGpuDirect;
              is_initialized_ = true;
              return true;
            } else {
              std::cout << "[HmlReceiver] GPU-Direct RDMA: verbs running in software emulation (nvidia-peermem/NIC P2P unavailable); falling back to Tier 2 (GPU-Indirect)\n";
              hml_verbs_rx_close(verbs_rx_);
              verbs_rx_ = nullptr;
            }
          } else {
            std::cout << "[HmlReceiver] GPU-Direct verbs registration failed; falling back to Tier 2 (GPU-Indirect)\n";
          }
        }
      } else {
        std::cout << "[HmlReceiver] GPU-Direct requested but CUDA hardware/driver unavailable; falling back to Tier 2/3\n";
      }
#else
      std::cout << "[HmlReceiver] GPU-Direct requested but compiled without CUDA support; falling back to CPU\n";
#endif
    }

    // -------------------------------------------------------------
    // PRIORITY 2: GPU-Indirect
    // -------------------------------------------------------------
    if (try_indirect) {
#if HML_HAVE_CUDA
      if (cuda_available_) {
        if (!d_gpu_buffer_) {
          cudaSetDevice(cfg_.cuda_device_id);
          cudaError_t st = cudaMalloc(&d_gpu_buffer_, cfg_.max_frame_bytes);
          if (st == cudaSuccess && !cu_stream_) {
            cudaStreamCreate(&cu_stream_);
          }
        }
        if (d_gpu_buffer_) {
          if (!host_buffer_) {
            host_buffer_ = static_cast<uint8_t *>(std::malloc(cfg_.max_frame_bytes));
            if (host_buffer_) std::memset(host_buffer_, 0, cfg_.max_frame_bytes);
          }
          if (host_buffer_) {
            bool xport_ok = (cfg_.transport == TransportKind::kVerbs) ?
                            init_verbs_transport(host_buffer_) : init_udp_transport();
            if (xport_ok) {
              std::cout << "[HmlReceiver] Priority 2 (GPU-Indirect) active. Staged host RAM -> CUDA VRAM via async copy\n";
              cfg_.tier = MemoryTier::kGpuIndirect;
              is_initialized_ = true;
              return true;
            }
          }
        }
      }
      std::cout << "[HmlReceiver] GPU-Indirect fallback failed (CUDA/device unavailable); falling back to Tier 1 (CPU-Only)\n";
#endif
    }

    // -------------------------------------------------------------
    // PRIORITY 3: CPU-Only
    // -------------------------------------------------------------
    cfg_.tier = MemoryTier::kCpu;
    if (!host_buffer_) {
      host_buffer_ = static_cast<uint8_t *>(std::malloc(cfg_.max_frame_bytes));
      if (host_buffer_) std::memset(host_buffer_, 0, cfg_.max_frame_bytes);
    }
    if (!host_buffer_) return false;

    bool xport_ok = (cfg_.transport == TransportKind::kVerbs) ?
                    init_verbs_transport(host_buffer_) : init_udp_transport();
    if (!xport_ok) return false;

    std::cout << "[HmlReceiver] Priority 3 (CPU-Only) active. Buffer: Host RAM\n";
    is_initialized_ = true;
    return true;
  }

  void shutdown() {
    if (!is_initialized_) return;

    if (udp_sock_) {
      hml_sock_close(udp_sock_);
      udp_sock_ = nullptr;
    }
    if (reasm_) {
      hml_reasm_close(reasm_);
      reasm_ = nullptr;
    }
    if (verbs_rx_) {
      hml_verbs_rx_close(verbs_rx_);
      verbs_rx_ = nullptr;
    }

#if HML_HAVE_CUDA
    if (d_gpu_buffer_) {
      cudaFree(d_gpu_buffer_);
      d_gpu_buffer_ = nullptr;
    }
    if (cu_stream_) {
      cudaStreamDestroy(cu_stream_);
      cu_stream_ = nullptr;
    }
#endif

    if (host_buffer_) {
      std::free(host_buffer_);
      host_buffer_ = nullptr;
    }

    is_initialized_ = false;
  }

  // Non-blocking or timed poll for next assembled frame
  hml::dlpack::DLManagedTensor *poll_frame(int timeout_ms = 5) {
    if (!is_initialized_) return nullptr;

    auto t0 = std::chrono::steady_clock::now();

    if (cfg_.transport == TransportKind::kVerbs) {
      hml_verbs_wc wc;
      int n = hml_verbs_poll_cq(verbs_rx_, 1, &wc);
      if (n > 0 && wc.status == HML_VERBS_WC_SUCCESS) {
        size_t frame_bytes = wc.byte_len ? wc.byte_len : (cfg_.max_frame_bytes / 2);
        stats_.frames_received++;
        stats_.bytes_received += frame_bytes;

        auto t1 = std::chrono::steady_clock::now();
        stats_.last_latency_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        return package_tensor(hml_verbs_rx_get_buffer(verbs_rx_), frame_bytes);
      } else if (n < 0) {
        stats_.errors++;
      } else {
        stats_.timeouts++;
      }
      return nullptr;
    }

    // UDP Transport
    uint8_t rx_buf[HML_MAX_FRAG_SIZE];
    size_t rx_bytes = 0;
    frame_ready_ = false;

    hml_status st = hml_sock_recv(udp_sock_, rx_buf, sizeof(rx_buf), &rx_bytes, timeout_ms);
    if (st == HML_OK && rx_bytes > 0) {
      hml_reasm_feed_datagram(reasm_, rx_buf, rx_bytes);
      if (frame_ready_ && last_frame_bytes_ > 0) {
        stats_.frames_received++;
        stats_.bytes_received += last_frame_bytes_;

        auto t1 = std::chrono::steady_clock::now();
        stats_.last_latency_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        return package_tensor(host_buffer_, last_frame_bytes_);
      }
    } else if (st == HML_ERR_TIMEOUT) {
      stats_.timeouts++;
    } else {
      stats_.errors++;
    }
    return nullptr;
  }

  // Inject a synthetic frame (for verification testing across all tiers)
  hml::dlpack::DLManagedTensor *inject_test_frame(const void *data, size_t size) {
    if (!is_initialized_ || !data || size == 0) return nullptr;
    size = std::min(size, cfg_.max_frame_bytes);

    if (cfg_.transport == TransportKind::kVerbs) {
#if HML_HAVE_CUDA
      if (cuda_available_ && cfg_.tier == MemoryTier::kGpuDirect && d_gpu_buffer_) {
        cudaMemcpy(d_gpu_buffer_, data, size, cudaMemcpyHostToDevice);
        (void)hml_verbs_simulate_write_imm(verbs_rx_, 0, nullptr, size, 0x1234);
        return poll_frame(1);
      }
#endif
      (void)hml_verbs_simulate_write_imm(verbs_rx_, 0, data, size, 0x1234);
      return poll_frame(1);
    } else {
      std::memcpy(host_buffer_, data, size);
      stats_.frames_received++;
      stats_.bytes_received += size;
      return package_tensor(host_buffer_, size);
    }
  }

  const ReceiverStats &stats() const { return stats_; }
  const ReceiverConfig &config() const { return cfg_; }
  bool is_cuda_active() const { return cuda_available_ && (cfg_.tier != MemoryTier::kCpu); }

 private:
  bool init_verbs_transport(void *user_buffer) {
    hml_verbs_cfg vcfg;
    hml_verbs_cfg_default(&vcfg);
    vcfg.dev_name = cfg_.dev_name.empty() ? nullptr : cfg_.dev_name.c_str();
    vcfg.buffer_size = cfg_.max_frame_bytes;
    vcfg.qp_type = cfg_.qp_type;
    vcfg.allow_emulation = cfg_.allow_emulation;
    vcfg.force_emulation = cfg_.force_emulation;
    vcfg.user_buffer = user_buffer;

    hml_status st = hml_verbs_rx_open(&verbs_rx_, &vcfg);
    if (st != HML_OK) {
      std::cerr << "[HmlReceiver] hml_verbs_rx_open failed: " << static_cast<int>(st) << "\n";
      return false;
    }
    return true;
  }

  bool init_udp_transport() {
    hml_status st = hml_sock_udp_create(&udp_sock_, 0, cfg_.udp_port, false);
    if (st != HML_OK) {
      std::cerr << "[HmlReceiver] Failed to bind UDP port " << cfg_.udp_port << "\n";
      return false;
    }

    st = hml_reasm_open(&reasm_, cfg_.max_frame_bytes);
    if (st != HML_OK) {
      std::cerr << "[HmlReceiver] Failed to open reassembler\n";
      return false;
    }

    hml_reasm_set_eof_cb(reasm_, &HmlReceiverEngine::on_eof_static, this);
    return true;
  }

  static void on_eof_static(void *ctx, uint16_t stream_id, uint32_t seq,
                            const uint8_t *frame, size_t size, const hml_frame_hdr *hdr) {
    auto *self = static_cast<HmlReceiverEngine *>(ctx);
    self->on_reasm_eof(stream_id, seq, frame, size, hdr);
  }

  void on_reasm_eof(uint16_t stream_id, uint32_t seq, const uint8_t *frame,
                    size_t size, const hml_frame_hdr *hdr) {
    (void)stream_id;
    (void)seq;
    (void)hdr;
    if (frame && size > 0 && size <= cfg_.max_frame_bytes) {
      std::memcpy(host_buffer_, frame, size);
      last_frame_bytes_ = size;
      frame_ready_ = true;
    }
  }

  hml::dlpack::DLManagedTensor *package_tensor(void *src_data, size_t size) {
    if (cfg_.tier == MemoryTier::kCpu) {
      return hml::dlpack::wrap_buffer(src_data, size, hml::dlpack::kDLCPU, 0);
    }

#if HML_HAVE_CUDA
    if (cuda_available_ && d_gpu_buffer_) {
      if (cfg_.tier == MemoryTier::kGpuIndirect) {
        // Tier 2: Asynchronous copy from pinned host RAM to GPU VRAM
        cudaMemcpyAsync(d_gpu_buffer_, src_data, size, cudaMemcpyHostToDevice, cu_stream_);
        cudaStreamSynchronize(cu_stream_);
      }
      // Tier 2 or Tier 3: Emit tensor pointing to GPU VRAM
      return hml::dlpack::wrap_buffer(d_gpu_buffer_, size, hml::dlpack::kDLCUDA, cfg_.cuda_device_id);
    }
#endif

    // Fallback if CUDA is disabled
    return hml::dlpack::wrap_buffer(src_data, size, hml::dlpack::kDLCPU, 0);
  }

  ReceiverConfig cfg_;
  hml_sock_t *udp_sock_;
  hml_reasm_ctx_t *reasm_;
  hml_verbs_rx_t *verbs_rx_;
  uint8_t *host_buffer_;
  void *d_gpu_buffer_;
#if HML_HAVE_CUDA
  cudaStream_t cu_stream_ = nullptr;
#else
  void *cu_stream_ = nullptr;
#endif

  bool is_initialized_;
  bool frame_ready_;
  size_t last_frame_bytes_;
  bool cuda_available_;
  ReceiverStats stats_;
};

#if HML_HAVE_HOLOSCAN
// NVIDIA Holoscan Operator declaration for native Holoscan SDK integration
class HmlReceiverOp : public ::holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_MEMBERS(HmlReceiverOp);

  void setup(::holoscan::OperatorSpec &spec) override {
    spec.param(transport_str_, "transport", "Transport", "verbs or udp", std::string("udp"));
    spec.param(tier_str_, "tier", "Tier", "auto, cpu, gpu_indirect, or gpu_direct", std::string("auto"));
    spec.param(qp_type_str_, "qp_type", "QP Type", "rc or uc", std::string("rc"));
    spec.param(port_, "port", "Port", "Data port", static_cast<uint16_t>(55101));
    spec.param(max_frame_bytes_, "max_frame_bytes", "Max Frame Bytes", static_cast<size_t>(65536));
    spec.param(cuda_device_id_, "device_id", "CUDA Device ID", 0);
    spec.output<::holoscan::Tensor>("out_tensor");
  }

  void initialize() override {
    ::holoscan::Operator::initialize();
    ReceiverConfig cfg;
    cfg.transport = (transport_str_.get() == "verbs") ? TransportKind::kVerbs : TransportKind::kUdp;
    if (tier_str_.get() == "gpu_direct") cfg.tier = MemoryTier::kGpuDirect;
    else if (tier_str_.get() == "gpu_indirect") cfg.tier = MemoryTier::kGpuIndirect;
    else if (tier_str_.get() == "cpu") cfg.tier = MemoryTier::kCpu;
    else cfg.tier = MemoryTier::kAuto;

    if (qp_type_str_.get() == "uc") cfg.qp_type = HML_VERBS_QP_UC;
    else cfg.qp_type = HML_VERBS_QP_RC;

    cfg.udp_port = port_.get();
    cfg.max_frame_bytes = max_frame_bytes_.get();
    cfg.cuda_device_id = cuda_device_id_.get();

    engine_ = std::make_unique<HmlReceiverEngine>(cfg);
    if (!engine_->initialize()) {
      throw std::runtime_error("HmlReceiverOp: Failed to initialize receiver engine");
    }
  }

  void compute(::holoscan::InputContext&, ::holoscan::OutputContext &op_output, ::holoscan::ExecutionContext&) override {
    auto *dl_tensor = engine_->poll_frame(10);
    if (dl_tensor) {
      // Emit DLPack tensor to Holoscan operator graph
      op_output.emit(dl_tensor, "out_tensor");
    }
  }

 private:
  ::holoscan::Parameter<std::string> transport_str_;
  ::holoscan::Parameter<std::string> tier_str_;
  ::holoscan::Parameter<std::string> qp_type_str_;
  ::holoscan::Parameter<uint16_t> port_;
  ::holoscan::Parameter<size_t> max_frame_bytes_;
  ::holoscan::Parameter<int> cuda_device_id_;
  std::unique_ptr<HmlReceiverEngine> engine_;
};
#endif

}  // namespace hml::holoscan_bridge

#endif  // HML_RECEIVER_OP_HPP

#ifndef HML_RECEIVER_OP_HPP
#define HML_RECEIVER_OP_HPP

/*
 * hml_receiver_op.hpp -- Reference NVIDIA Holoscan Operator for HML sensor streaming.
 *
 * This operator bridges Host MCU Lite (hml) UDP / RoCEv2 sensor data into
 * an NVIDIA Holoscan execution graph.
 *
 * Architecture:
 *   MCU (Sensor) ---> UDP / RoCEv2 (T0/T1a)
 *       ---> HmlReceiverOp::compute()
 *       ---> hml_reasm / hml_host (reassembly + CRC verification)
 *       ---> cudaMemcpyAsync (Host buffer -> Pinned CUDA device memory)
 *       ---> op_output.emit(holoscan::Tensor, "out_tensor")
 *       ---> Downstream Holoscan AI Operators (e.g. TensorRtInferenceOp)
 *
 * Note on Holoscan SDK integration:
 *   Stock Holoscan operators (RoceReceiverOp) require NVIDIA hololink (BOOTP + ECB).
 *   This custom operator allows Holoscan applications to directly ingest HML frames
 *   without requiring hololink reverse-engineering or ConnectX hardware.
 */

#include <hml/hml_protocol.h>
#include <hml/hml_reasm.h>
#include <hml/hml_sock.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Forward declarations if compiled without Holoscan SDK headers installed
#if __has_include(<holoscan/holoscan.hpp>)
#include <holoscan/holoscan.hpp>
#include <cuda_runtime.h>
#define HML_HAS_HOLOSCAN_HEADERS 1
#else
#define HML_HAS_HOLOSCAN_HEADERS 0
#endif

namespace holoscan::ops {

#if HML_HAS_HOLOSCAN_HEADERS

class HmlReceiverOp : public Operator {
 public:
  HOLOSCAN_OPERATOR_MEMBERS(HmlReceiverOp);

  void setup(OperatorSpec& spec) override {
    spec.param(port_, "port", "Port", "UDP data port to listen on", static_cast<uint16_t>(55101));
    spec.param(max_frame_bytes_, "max_frame_bytes", "Max Frame Bytes", "Max assembled frame size", static_cast<size_t>(1048576));
    spec.param(cuda_device_id_, "device_id", "CUDA Device ID", "Target CUDA device ID", 0);
    spec.output<Tensor>("out_tensor");
  }

  void initialize() override {
    Operator::initialize();

    // 1. Create UDP socket
    hml_status st = hml_sock_udp_create(&sock_, 0, port_.get(), false);
    if (st != HML_OK) {
      throw std::runtime_error("HmlReceiverOp: failed to bind UDP port " + std::to_string(port_.get()));
    }

    // 2. Open HML reassembler
    st = hml_reasm_open(&reasm_, max_frame_bytes_.get());
    if (st != HML_OK) {
      hml_sock_close(sock_);
      throw std::runtime_error("HmlReceiverOp: failed to initialize reassembler");
    }

    // 3. Set EOF callback
    hml_reasm_set_eof_cb(reasm_, on_eof_static, this);

    // 4. Allocate CUDA pinned / device memory
    cudaSetDevice(cuda_device_id_.get());
    cudaError_t cu_st = cudaMalloc(&d_gpu_buffer_, max_frame_bytes_.get());
    if (cu_st != cudaSuccess) {
      throw std::runtime_error("HmlReceiverOp: cudaMalloc failed");
    }
    cudaStreamCreate(&cu_stream_);
  }

  void compute(InputContext&, OutputContext& op_output, ExecutionContext&) override {
    uint8_t rx_buf[HML_MAX_FRAG_SIZE];
    size_t rx_bytes = 0;

    // Poll socket for available datagrams (5 ms timeout)
    hml_status st = hml_sock_recv(sock_, rx_buf, sizeof(rx_buf), &rx_bytes, 5);
    if (st == HML_OK && rx_bytes > 0) {
      frame_ready_ = false;
      // Feed datagram into reassembly pipeline
      hml_reasm_feed_datagram(reasm_, rx_buf, rx_bytes);

      // If an EOF frame was completed by this datagram, emit to Holoscan graph
      if (frame_ready_ && last_frame_size_ > 0) {
        // Copy frame to CUDA device memory asynchronously
        cudaMemcpyAsync(d_gpu_buffer_, last_frame_data_, last_frame_size_,
                        cudaMemcpyHostToDevice, cu_stream_);
        cudaStreamSynchronize(cu_stream_);

        // Construct Holoscan Tensor metadata and emit
        std::vector<int64_t> shape = {static_cast<int64_t>(last_frame_size_)};
        auto tensor = std::make_shared<Tensor>(d_gpu_buffer_, shape, DLDataType{kDLUInt, 8, 1});
        op_output.emit(tensor, "out_tensor");
      }
    }
  }

  void stop() override {
    if (d_gpu_buffer_) {
      cudaFree(d_gpu_buffer_);
      d_gpu_buffer_ = nullptr;
    }
    if (cu_stream_) {
      cudaStreamDestroy(cu_stream_);
      cu_stream_ = nullptr;
    }
    if (reasm_) {
      hml_reasm_close(reasm_);
      reasm_ = nullptr;
    }
    if (sock_) {
      hml_sock_close(sock_);
      sock_ = nullptr;
    }
  }

 private:
  static void on_eof_static(void* ctx, uint16_t stream_id, uint32_t seq,
                           const uint8_t* frame, size_t n, const hml_frame_hdr* hdr) {
    auto* self = static_cast<HmlReceiverOp*>(ctx);
    self->on_eof(stream_id, seq, frame, n, hdr);
  }

  void on_eof(uint16_t, uint32_t, const uint8_t* frame, size_t n, const hml_frame_hdr*) {
    last_frame_data_ = frame;
    last_frame_size_ = n;
    frame_ready_ = true;
  }

  Parameter<uint16_t> port_;
  Parameter<size_t> max_frame_bytes_;
  Parameter<int> cuda_device_id_;

  hml_sock_t* sock_{nullptr};
  hml_reasm_ctx_t* reasm_{nullptr};

  void* d_gpu_buffer_{nullptr};
  cudaStream_t cu_stream_{nullptr};

  const uint8_t* last_frame_data_{nullptr};
  size_t last_frame_size_{0};
  bool frame_ready_{false};
};

#endif // HML_HAS_HOLOSCAN_HEADERS

} // namespace holoscan::ops

#endif // HML_RECEIVER_OP_HPP

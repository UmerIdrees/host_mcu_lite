/*
 * NVIDIA Holoscan AI Pipeline Example with HML Multi-Tier Receiver.
 *
 * Demonstrates:
 *   - Task C.1: Dual-transport ingestion (UDP and verbs RoCEv2 with UC and RC QP)
 *   - Task C.2: Three-tier memory architecture (CPU, GPU-Indirect, GPU-Direct)
 *   - Task C.3: DLPack TensorMap generation and downstream processing
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "examples/holoscan_operator/hml_receiver_op.hpp"

using hml::holoscan_bridge::HmlReceiverEngine;
using hml::holoscan_bridge::ReceiverConfig;
using hml::holoscan_bridge::TransportKind;
using hml::holoscan_bridge::MemoryTier;
using hml::dlpack::DLManagedTensor;
using hml::dlpack::kDLCPU;
using hml::dlpack::kDLCUDA;

// Downstream AI inference simulation operator
class MockInferenceOp {
 public:
  explicit MockInferenceOp(const std::string &model_name = "sensor_resnet18")
      : model_name_(model_name), processed_count_(0) {}

  bool process_tensor(DLManagedTensor *tensor, uint64_t frame_seq) {
    if (!tensor) return false;

    auto &dl = tensor->dl_tensor;
    const char *dev_name = (dl.device.device_type == kDLCUDA) ? "CUDA (GPU VRAM)" : "CPU (Host RAM)";
    size_t num_bytes = dl.shape[0];

    // Compute simple checksum or verify first bytes if accessible on CPU
    uint32_t sample = 0;
    if (dl.device.device_type == kDLCPU && dl.data) {
      sample = *reinterpret_cast<const uint32_t *>(dl.data);
    }

    std::cout << "  [InferenceOp] Processed frame #" << frame_seq
              << " | Device: " << dev_name
              << " (id: " << dl.device.device_id << ")"
              << " | Size: " << num_bytes << " B"
              << " | Sample: 0x" << std::hex << sample << std::dec << "\n";

    processed_count_++;
    return true;
  }

  uint64_t processed_count() const { return processed_count_; }

 private:
  std::string model_name_;
  uint64_t processed_count_;
};

static void print_usage(const char *prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "  --transport <udp|verbs>       Transport mode (default: udp)\n"
            << "  --tier <auto|cpu|gpu_indirect|gpu_direct>  Target memory tier (default: auto)\n"
            << "  --qp-type <rc|uc>             Verbs Queue Pair type (default: rc)\n"
            << "  --port <port>                 Data port (default: 55101)\n"
            << "  --frames <N>                  Frames to process in self-test (default: 10)\n"
            << "  --size <bytes>                Frame size in bytes (default: 4096)\n"
            << "  --test                        Run self-contained pipeline test (default)\n"
            << "  --live                        Listen for live incoming network frames\n"
            << "  --help                        Show this message\n";
}

int main(int argc, char **argv) {
  ReceiverConfig cfg;
  cfg.transport = TransportKind::kUdp;
  cfg.tier = MemoryTier::kAuto;
  cfg.qp_type = HML_VERBS_QP_RC;
  cfg.udp_port = 55101;
  cfg.max_frame_bytes = 65536;

  int num_frames = 10;
  size_t frame_size = 4096;
  bool run_test = true;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--transport" && i + 1 < argc) {
      std::string t = argv[++i];
      if (t == "verbs") cfg.transport = TransportKind::kVerbs;
      else cfg.transport = TransportKind::kUdp;
    } else if (arg == "--tier" && i + 1 < argc) {
      std::string m = argv[++i];
      if (m == "auto") cfg.tier = MemoryTier::kAuto;
      else if (m == "gpu_direct") cfg.tier = MemoryTier::kGpuDirect;
      else if (m == "gpu_indirect") cfg.tier = MemoryTier::kGpuIndirect;
      else if (m == "cpu") cfg.tier = MemoryTier::kCpu;
      else cfg.tier = MemoryTier::kAuto;
    } else if (arg == "--qp-type" && i + 1 < argc) {
      std::string q = argv[++i];
      if (q == "uc" || q == "UC") cfg.qp_type = HML_VERBS_QP_UC;
      else cfg.qp_type = HML_VERBS_QP_RC;
    } else if (arg == "--port" && i + 1 < argc) {
      cfg.udp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--frames" && i + 1 < argc) {
      num_frames = std::atoi(argv[++i]);
    } else if (arg == "--size" && i + 1 < argc) {
      frame_size = static_cast<size_t>(std::atoi(argv[++i]));
    } else if (arg == "--test") {
      run_test = true;
    } else if (arg == "--live") {
      run_test = false;
    }
  }

  std::cout << "========================================================\n"
            << "  NVIDIA Holoscan Multi-Tier Ingestion Bridge\n"
            << "========================================================\n"
            << " Transport : " << (cfg.transport == TransportKind::kVerbs ? "Verbs RoCEv2" : "Framed UDP (T0)") << "\n"
            << " TargetTier: " << (cfg.tier == MemoryTier::kAuto ? "Auto (Priority: GPU-Direct -> GPU-Indirect -> CPU)" :
                                 (cfg.tier == MemoryTier::kGpuDirect ? "Tier 3 (GPU-Direct)" :
                                 (cfg.tier == MemoryTier::kGpuIndirect ? "Tier 2 (GPU-Indirect)" : "Tier 1 (CPU-Only)"))) << "\n"
            << " QP Type   : " << (cfg.qp_type == HML_VERBS_QP_RC ? "Reliable Connected (RC)" : "Unreliable Connected (UC)") << "\n"
            << " Buffer Cap: " << cfg.max_frame_bytes << " B\n"
            << " Mode      : " << (run_test ? "Self-Test Pipeline" : "Live Ingestion Server") << "\n"
            << "========================================================\n";

  HmlReceiverEngine receiver(cfg);
  if (!receiver.initialize()) {
    std::cerr << "Failed to initialize HmlReceiverEngine!\n";
    return 1;
  }

  std::cout << " Negotiated Active Tier: "
            << (receiver.config().tier == MemoryTier::kGpuDirect ? "Tier 3 (GPU-Direct RDMA)" :
               (receiver.config().tier == MemoryTier::kGpuIndirect ? "Tier 2 (GPU-Indirect DMA)" : "Tier 1 (CPU-Only)")) << "\n\n";

  MockInferenceOp inference("holoscan_ai_pipeline");

  if (run_test) {
    std::cout << "Running end-to-end pipeline ingestion test with "
              << num_frames << " frames (" << frame_size << " bytes each)...\n\n";

    std::vector<uint8_t> payload(frame_size);
    auto start_time = std::chrono::steady_clock::now();

    for (int f = 0; f < num_frames; ++f) {
      // Create synthetic test frame with pattern
      for (size_t b = 0; b < frame_size; ++b) {
        payload[b] = static_cast<uint8_t>((f * 17 + b) & 0xFF);
      }

      DLManagedTensor *tensor = receiver.inject_test_frame(payload.data(), payload.size());
      if (!tensor) {
        std::cerr << "Error: Failed to ingest frame #" << f << "\n";
        continue;
      }

      // Execute downstream operator
      inference.process_tensor(tensor, f);

      // Release DLManagedTensor
      if (tensor->deleter) {
        tensor->deleter(tensor);
      }
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    double total_mb = static_cast<double>(num_frames * frame_size) / (1024.0 * 1024.0);
    double throughput_mbps = (elapsed_ms > 0) ? (total_mb / (elapsed_ms / 1000.0)) : 0.0;

    std::cout << "\n========================================================\n"
              << " Pipeline Execution Summary\n"
              << "========================================================\n"
              << " Frames Ingested : " << receiver.stats().frames_received << " / " << num_frames << "\n"
              << " Total Bytes     : " << receiver.stats().bytes_received << " B\n"
              << " Duration        : " << elapsed_ms << " ms\n"
              << " Throughput      : " << throughput_mbps << " MB/s\n"
              << " Errors          : " << receiver.stats().errors << "\n"
              << " Status          : " << ((receiver.stats().frames_received == static_cast<uint64_t>(num_frames)) ? "PASS (100% Zero-Drop)" : "FAIL") << "\n"
              << "========================================================\n";

    return (receiver.stats().frames_received == static_cast<uint64_t>(num_frames)) ? 0 : 1;
  } else {
    std::cout << "Listening for incoming sensor frames on port " << cfg.udp_port << "... (Ctrl+C to stop)\n";
    uint64_t frame_seq = 0;
    while (true) {
      DLManagedTensor *tensor = receiver.poll_frame(100);
      if (tensor) {
        inference.process_tensor(tensor, frame_seq++);
        if (tensor->deleter) {
          tensor->deleter(tensor);
        }
      }
    }
  }

  return 0;
}

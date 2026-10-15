#include "hml_receiver_op.hpp"

namespace hml::holoscan_bridge {

// Explicit instantiation and symbol exports for shared/static consumers
const char *transport_kind_to_string(TransportKind k) {
  switch (k) {
    case TransportKind::kUdp: return "udp";
    case TransportKind::kVerbs: return "verbs";
    default: return "unknown";
  }
}

const char *memory_tier_to_string(MemoryTier t) {
  switch (t) {
    case MemoryTier::kAuto: return "auto";
    case MemoryTier::kCpu: return "cpu";
    case MemoryTier::kGpuIndirect: return "gpu_indirect";
    case MemoryTier::kGpuDirect: return "gpu_direct";
    default: return "unknown";
  }
}

} // namespace hml::holoscan_bridge

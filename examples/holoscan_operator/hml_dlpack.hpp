#ifndef HML_DLPACK_HPP
#define HML_DLPACK_HPP

/*
 * hml_dlpack.hpp -- Minimal, standard DLPack representation for NVIDIA Holoscan
 * and cross-framework Tensor exchange.
 *
 * Conforms to the standard DLPack specification (dlpack/dlpack.h) to allow
 * zero-copy sharing between HmlReceiverOp, Holoscan Tensors, PyTorch, and TensorRT.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace hml::dlpack {

enum DLDeviceType {
  kDLCPU = 1,
  kDLCUDA = 2,
  kDLCUDAHost = 3,
  kDLOpenCL = 4,
  kDLVulkan = 7,
  kDLMetal = 8,
  kDLVPI = 9,
  kDLROCM = 10,
  kDLOneAPI = 14,
};

struct DLDevice {
  DLDeviceType device_type;
  int32_t device_id;
};

enum DLDataTypeCode {
  kDLInt = 0,
  kDLUInt = 1,
  kDLFloat = 2,
  kDLOpaqueHandle = 3,
  kDLBfloat = 4,
  kDLComplex = 5,
};

struct DLDataType {
  uint8_t code;
  uint8_t bits;
  uint16_t lanes;
};

struct DLTensor {
  void *data;
  DLDevice device;
  int32_t ndim;
  DLDataType dtype;
  int64_t *shape;
  int64_t *strides;
  uint64_t byte_offset;
};

struct DLManagedTensor {
  DLTensor dl_tensor;
  void *manager_ctx;
  void (*deleter)(struct DLManagedTensor *self);
};

/* Helper to allocate and wrap a raw 1D byte buffer into a DLManagedTensor */
inline DLManagedTensor *wrap_buffer(void *data, size_t size, DLDeviceType dev_type, int dev_id = 0) {
  auto *ctx = new DLManagedTensor();
  ctx->dl_tensor.data = data;
  ctx->dl_tensor.device = DLDevice{dev_type, dev_id};
  ctx->dl_tensor.ndim = 1;
  ctx->dl_tensor.dtype = DLDataType{kDLUInt, 8, 1};  // uint8

  auto *shape = new int64_t[1];
  shape[0] = static_cast<int64_t>(size);
  ctx->dl_tensor.shape = shape;

  auto *strides = new int64_t[1];
  strides[0] = 1;
  ctx->dl_tensor.strides = strides;

  ctx->dl_tensor.byte_offset = 0;
  ctx->manager_ctx = nullptr;
  ctx->deleter = [](DLManagedTensor *self) {
    if (self) {
      delete[] self->dl_tensor.shape;
      delete[] self->dl_tensor.strides;
      delete self;
    }
  };
  return ctx;
}

}  // namespace hml::dlpack

#endif  // HML_DLPACK_HPP

# Linux Server & Workstation Bring-Up Guide

This guide documents the procedure to bring up a Linux workstation or server with `holoscan_mcu_lite`, configuring Soft-RoCE (`rdma_rxe`), physical NVIDIA GPU memory (Tier 2 GPU-Indirect DMA), and the Renode Cortex-M7 emulation environment.

---

## 1. System Requirements & Architecture

* **Host Architecture**: x86_64 (Linux 6.x or 7.x kernels)
* **Memory**: 8 GB+ RAM
* **GPU**: Any NVIDIA CUDA-capable GPU (GeForce GTX/RTX, Quadro, or Data Center)
* **Network**: Any standard NIC or Linux loopback interface (`lo`) for Soft-RoCE

### Tier 2 (GPU-Indirect RDMA) Architecture

* **GPUDirect RDMA (Tier 3)** requires enterprise hardware (NVIDIA ConnectX NICs + Data Center GPUs with PCIe peer-to-peer BAR1 mapping via `nvidia-peermem`). Consumer GeForce GPUs do not support GPUDirect RDMA.
* **GPU-Indirect RDMA (Tier 2)** allows consumer and workstation GPUs (e.g., GeForce GTX/RTX series) to ingest high-throughput RoCEv2 streams by writing packets into pinned Host RAM via Soft-RoCE, then staging directly into GPU VRAM using `cudaMemcpyAsync()`.

```
[MCU / Renode / Sensor]
           │  (RoCEv2 over Ethernet / Loopback)
           ▼
[Linux Soft-RoCE (rdma_rxe) / ibverbs]
           │  (RDMA Write to Host Memory Region)
           ▼
     [Host RAM (CPU)]
           │  (cudaMemcpyAsync over PCIe)
           ▼
     [NVIDIA GPU VRAM]
           │  (Zero-copy DLPack kDLCUDA Tensor)
           ▼
 [Holoscan / AI Inference Operator]
```

---

## 2. Environment & Dependency Setup

### Step 2.1: Core Packages & Toolchains
Install the base build tools, RDMA libraries, CUDA toolkit, and ARM cross-compiler:

```bash
sudo apt update

# 1. Base build environment and RDMA user space
# NOTE: The package is 'ibverbs-utils', NOT 'libibverbs-utils'.
sudo apt install -y cmake build-essential gcc g++ rdma-core ibverbs-utils libibverbs-dev iproute2

# 2. CUDA toolkit & headers (enables GPU VRAM allocation & DLPack kDLCUDA tensors)
sudo apt install -y nvidia-cuda-toolkit

# 3. ARM cross-compiler (needed for Renode Cortex-M7 firmware compilation)
sudo apt install -y gcc-arm-none-eabi libnewlib-arm-none-eabi
```

### Step 2.2: Soft-RoCE Kernel Setup
On modern kernels (Linux 6.x / 7.x), the `rdma_rxe` driver is built into the kernel tree.

```bash
# 1. Load kernel module
sudo modprobe rdma_rxe

# 2. Add RoCE link on loopback (or a physical NIC, e.g. eth0)
sudo rdma link add rxe0 type rxe netdev lo

# 3. Verify RoCE device is active
ibv_devinfo
```

### Step 2.3: Renode Emulation & lwIP Setup

To run the Renode MCU emulation tests locally, users can configure their own standalone Renode environment:

#### 1. Install Renode Portable
```bash
# Download and install Renode portable (e.g. in /opt/renode or ~/renode)
sudo mkdir -p /opt/renode
cd /opt/renode
sudo wget https://github.com/renode/renode/releases/download/v1.16.0/renode-1.16.0.linux-portable.tar.gz
sudo tar -xzf renode-1.16.0.linux-portable.tar.gz --strip-components=1

# Symlink binaries to system PATH
sudo ln -sf /opt/renode/renode /usr/local/bin/renode
sudo ln -sf /opt/renode/renode-test /usr/local/bin/renode-test
```

#### 2. Configure Python Virtual Environment for Renode Test
Renode Robot Framework testing requires `robotframework`, `psutil`, and `pyyaml`:
```bash
python3 -m venv ~/.renode_venv
~/.renode_venv/bin/pip install robotframework psutil pyyaml

# Export environment variable pointing to the venv bin directory:
export HML_RENODE_PYTHON_PATH="$HOME/.renode_venv/bin"
```

#### 3. Configure lwIP for Cortex-M7 Firmware Emulation
The STM32F767ZI network emulation test compiles against a standalone lwIP core (`liblwip.a`):
```bash
# Point HML_LWIP_BASE to your local lwIP installation directory containing build/liblwip.a:
export HML_LWIP_BASE="$HOME/toolchains/lwip"

# Ensure the library file exists at:
# $HML_LWIP_BASE/build/liblwip.a
```
*(If `$HML_LWIP_BASE` is not set, the harness defaults to checking `$HOME/hml_toolchain_cache/lwip`).*

---

## 3. Building the Project

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## 4. Verification & Testing Playbook

### 1. GPU VRAM Ingestion Verification
Verifies end-to-end multi-tier pipeline execution with physical CUDA VRAM allocation:
```bash
./build/examples/holoscan_ai_pipeline/holoscan_ai_pipeline \
  --transport verbs \
  --qp-type rc \
  --tier gpu_indirect \
  --frames 20 \
  --test
```
*Expected log assertion:*
```text
[HmlReceiver] Priority 2 (GPU-Indirect) active. Staged host RAM -> CUDA VRAM via async copy
Negotiated Active Tier: Tier 2 (GPU-Indirect DMA)
  [InferenceOp] Processed frame #0 | Device: CUDA (GPU VRAM) (id: 0) | Size: 4096 B
Status : PASS (100% Zero-Drop)
```

### 2. Live Network Ingestion Stream
* **Terminal 1 (Server):**
  ```bash
  ./build/examples/holoscan_ai_pipeline/holoscan_ai_pipeline --transport udp --tier gpu_indirect --live
  ```
* **Terminal 2 (Client / Generator):**
  ```bash
  ./build/adapters/synthetic/hml_gen --port 55101 --frames 50 --size 4096 --seed 1
  ```
* **Terminal 3 (Monitor):** Run `nvidia-smi` to observe the pipeline active under GPU processes.

### 3. Integrated Renode MCU to Holoscan E2E Test
Cross-compiles Cortex-M7 firmware, emulates the Synopsys MAC physical wire, resolves ARP, and ingests packets into Holoscan:
```bash
./tests/matrix/test_renode_holoscan_e2e.sh
```
*Expected: 4/4 Stages PASSED.*

### 4. 6-Way Holoscan Matrix Test
Validates all transport-tier combinations (UDP/RoCEv2 RC/UC across CPU, GPU-Indirect, and GPU-Direct probe):
```bash
./tests/matrix/test_6way_matrix.sh
```
*Expected: 6/6 Tests PASSED.*

### 5. Master CTest Suite
```bash
ctest --test-dir build --output-on-failure
```
*Expected: 100% tests passed (headless GUI smoke tests skip cleanly with code 77).*

---

## 5. Technical Insights & Architecture Notes

1. **Consumer GPU vs Enterprise GPUDirect RDMA**:
   * True Tier 3 (GPUDirect RDMA) requires hardware peer-to-peer PCIe BAR1 mapping (`nvidia-peermem`) and is restricted to enterprise GPUs paired with NVIDIA ConnectX NICs.
   * Consumer GPUs achieve high performance and full NVIDIA Holoscan pipeline compatibility via **Tier 2 (GPU-Indirect RDMA)**. RoCEv2/UDP datagrams are received into registered host memory and transferred to GPU VRAM via `cudaMemcpyAsync()`, surfaced downstream as standard DLPack `kDLCUDA` tensors (`Device: CUDA (GPU VRAM) (id: 0)`).

2. **Hardware vs Software Completion Queue Reaping in Native Verbs**:
   * When native verbs devices (`rxe0` or physical HCAs) are active, `hml_verbs_poll_cq()` must check software CQ queues (`rx->cqe_count > 0`) before querying the kernel hardware completion queue (`ibv_poll_cq()`), ensuring test-injected frames are never missed during pipeline verification.

3. **Dynamic Hardware Acceleration Fallback**:
   * When compiled with libibverbs support on a system with no active RDMA devices configured, `hml_accel_backend()` checks for discovered devices and gracefully falls back to `"cpu"` instead of raising hard errors.

4. **`libibverbs` Memory Region Access Flags**:
   * Receive memory regions for RDMA operations require `IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE`. Standard `<infiniband/verbs.h>` does not define `IBV_ACCESS_RECV`.

5. **Debian/Ubuntu Package Naming**:
   * The package containing `ibv_devinfo` in Debian and Ubuntu repositories is named `ibverbs-utils`, not `libibverbs-utils`.

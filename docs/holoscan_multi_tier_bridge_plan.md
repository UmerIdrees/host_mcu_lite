# Implementation Plan: NVIDIA Holoscan Multi-Tier Ingestion Bridge with Soft-RoCE & Dual Transport

## Goal Description

Enable an embedded microcontroller (MCU) to stream sensor data over **both RDMA (RoCEv2)** and **Framed UDP (T0)** to a host server running an **NVIDIA Holoscan** application. The host side must support three distinct acceleration tiers (**CPU-only**, **GPU-indirect**, and **GPU-direct RDMA**) without requiring modifications to the stock Holoscan SDK. In the absence of a physical ConnectX NIC, Linux **Soft-RoCE (`rdma_rxe`)** serves as the verifiable reference platform. The RDMA control path uses lightweight custom sockets on both the MCU and server.

---

## Architecture & Tier Capability Matrix

```
                      ┌──────────────────────────────────────────────┐
                      │            CUSTOM SOCKET CONTROL             │
                      │  Server (hml_client) <===UDP 55102===> MCU   │
                      │  Negotiates: Transport, QP, rkey, VA, START  │
                      └──────────────────────────────────────────────┘
                                              │
                      ┌───────────────────────┴──────────────────────┐
                      ▼                                              ▼
          [ DATA PATH: RoCEv2 (T1a) ]                    [ DATA PATH: UDP (T0) ]
          UDP Port 4791 / BTH / RETH                     UDP Port 55101 / MTU 1458
                      │                                              │
         ┌────────────┴────────────┐                                 │
         ▼                         ▼                                 │
   Physical NIC             Soft-RoCE (rxe0)                         │
   (ConnectX)               (Kernel Reference)                       │
         │                         │                                 │
         └────────────┬────────────┘                                 │
                      ▼                                              ▼
          ┌───────────────────────┐                      ┌───────────────────────┐
          │  Kernel libibverbs    │                      │  hml_sock / hml_reasm │
          │  Queue Pair & CQE     │                      │  Window Reassembly    │
          └───────────┬───────────┘                      └───────────┬───────────┘
                      │                                              │
                      └──────────────────────┬───────────────────────┘
                                             ▼
                        ┌────────────────────────────────────────┐
                        │      Custom Holoscan HmlReceiverOp     │
                        │                                        │
                        │  Tier 1: CPU-Only (Host RAM DLPack)    │
                        │  Tier 2: GPU-Indirect (cudaMemcpyAsync)│
                        │  Tier 3: GPU-Direct (Direct VRAM DMA)  │
                        └────────────────────┬───────────────────┘
                                             ▼ holoscan::TensorMap
                        ┌────────────────────────────────────────┐
                        │ Stock Holoscan AI Operators            │
                        │ (TensorRtInferenceOp, Holoviz, etc.)   │
                        └────────────────────────────────────────┘
```

### Full Configuration Matrix

| Ingestion Mode | Transport | Server Receiver | Memory Target | Holoscan DLDevice | Direct Hardware Req |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Mode 1A: CPU-Only (UDP)** | T0 Framed UDP | `hml_sock` + `hml_reasm` | System Heap | `kDLCPU, 0` | None (POSIX Sockets) |
| **Mode 1B: CPU-Only (RoCE)** | T1a RoCEv2 | `libibverbs` on `rxe0` | Pinned Host RAM (`ibv_reg_mr`) | `kDLCPU, 0` | Linux `rdma_rxe` |
| **Mode 2A: GPU-Indirect (UDP)**| T0 Framed UDP | `hml_sock` + `hml_reasm` | Host -> `cudaMemcpyAsync` | `kDLCUDA, dev_id`| NVIDIA GPU / CUDA |
| **Mode 2B: GPU-Indirect (RoCE)**| T1a RoCEv2 | `libibverbs` on `rxe0` | Pinned Host -> `cudaMemcpyAsync` | `kDLCUDA, dev_id`| `rdma_rxe` + NVIDIA GPU |
| **Mode 3: GPU-Direct (RoCE)** | T1a RoCEv2 | `libibverbs` on `rxe0`/NIC | GPU BAR1 (`nvidia-peermem`) | `kDLCUDA, dev_id`| GPU Direct RDMA |

---

## User Review Required

> [!IMPORTANT]
> ### 1. Reference Software RoCE (`rdma_rxe`) Environment
> In the absence of a physical Mellanox ConnectX card, Soft-RoCE (`rdma_rxe`) operates directly on virtual ethernet interfaces (`veth`). Kernel modules `rdma_rxe` and user packages `rdma-core` / `libibverbs-dev` will be used as the automated reference test harness.

> [!IMPORTANT]
> ### 2. Custom Socket Control Handshake
> Both MCU and server will speak HML's 11-opcode control protocol on UDP port 55102:
> 1. Host server allocates memory and Queue Pair.
> 2. Host server queries local GID/QP, formats `hml_dest_cfg`, and transmits `HML_OP_SET_DEST` via custom UDP socket to the MCU.
> 3. Host server sends `HML_OP_START` over socket.
> 4. MCU begins streaming data over the selected transport (RoCEv2 or UDP).

> [!WARNING]
> ### 3. GPU-Direct vs. GPU-Indirect Prerequisites
> GPU-Direct RDMA requires `nvidia-peermem` kernel module and GPU BAR1 PCIe support. On hosts where `nvidia-peermem` is not loaded, `HmlReceiverOp` will automatically fall back to **GPU-Indirect** (pinned host memory + `cudaMemcpyAsync`) with zero user intervention.

---

## Milestones & Parallel Work Breakdown

We have structured the development into **4 Milestones** featuring **3 independent parallel development tracks** that can be executed concurrently before full system integration.

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│ TRACK A: MCU Firmware & Transports          TRACK B: Server RDMA & Soft-RoCE           │
│ - Task A.1: RoCE Immediate Signalling       - Task B.1: Soft-RoCE Test Harness         │
│ - Task A.2: Custom Socket Control (MCU)     - Task B.2: Verbs Engine (libibverbs)      │
│                                             - Task B.3: Custom Socket Client (Host)    │
└───────────────────────────┬────────────────────────────────────────────┬───────────────┘
                            │                                            │
                            └─────────────────────┬──────────────────────┘
                                                  ▼
                    ┌────────────────────────────────────────────────────────────┐
                    │ TRACK C: NVIDIA Holoscan Multi-Tier Custom Operator        │
                    │ - Task C.1: Multi-Transport Engine (Verbs + UDP)           │
                    │ - Task C.2: Multi-Tier Memory (CPU, Indirect, Direct)      │
                    │ - Task C.3: Holoscan Application & DLPack Emission         │
                    └─────────────────────────────┬──────────────────────────────┘
                                                  ▼
                    ┌────────────────────────────────────────────────────────────┐
                    │ TRACK D: End-to-End System Integration & Matrix Tests      │
                    │ - Task D.1: 6-Way Verification Matrix Execution            │
                    │ - Task D.2: Documentation & Developer Quickstarts          │
                    └────────────────────────────────────────────────────────────┘
```

---

### Milestone 1: Transport Core & Control Handshake (Parallel Execution)

#### Track A: Embedded MCU Side (Can run in parallel with Track B & C)
* **Task A.1 (MCU RoCEv2 Immediate Signaling):**
  * File: `src/transports/rocev2/hml_rocev2.c`
  * Add support for `IBV_WR_RDMA_WRITE_WITH_IMM` (BTH OpCode `0x2B`) on the closing fragment so the receiving kernel RoCE device generates a Completion Queue Entry (CQE) upon frame completion.
* **Task A.2 (MCU Custom Socket Control Engine):**
  * File: `src/control/hml_ctrl_device.c`, `include/hml/hml_ctrl.h`
  * Extend `SET_DEST` handling on UDP port 55102 to receive dynamic server parameters: destination IP, transport mode (UDP vs RoCE), QP number, `rkey`, remote virtual address, and frame size.

#### Track B: Server RDMA & Soft-RoCE Reference Engine (Can run in parallel with Track A & C)
* **Task B.1 (Soft-RoCE Reference Lab Automation):**
  * File: `host/soft_roce/`
  * Automate non-interactive setup of `rxe0` on loopback/veth for continuous CI testing without physical hardware.
* **Task B.2 (Server Verbs Receiver Engine):**
  * Files: [NEW] `host/verbs_rx/hml_verbs_rx.h`, [NEW] `host/verbs_rx/hml_verbs_rx.c`
  * Complete full `libibverbs` lifecycle:
    * `ibv_open_device()` on `rxe0` (or `mlx5_0`).
    * Allocate Protection Domain (`ibv_alloc_pd`).
    * Allocate and transition Unreliable Connected (UC) Queue Pair (`RESET -> INIT -> RTR -> RTS`).
    * Implement `hml_verbs_poll_cq()` for non-blocking work completion detection on incoming Immediate data.
* **Task B.3 (Server Custom Socket Control Client):**
  * File: `src/control/hml_client.c`
  * Implement automated control client that binds socket, connects to MCU UDP port 55102, exchanges verbs QP/MR parameters, and fires `START` streaming command.

---

### Milestone 2: Multi-Tier Holoscan Custom Operator (`HmlReceiverOp`)

#### Track C: Holoscan Operator & Compute Graph
* **Task C.1 (Dual-Transport Ingestion Engine):**
  * Files: `examples/holoscan_operator/hml_receiver_op.hpp`, `examples/holoscan_operator/hml_receiver_op.cpp`
  * Implement configurable `transport` parameter (`"verbs"` vs `"udp"`):
    * If `"verbs"`: Polls `hml_verbs_poll_cq()` from Task B.2.
    * If `"udp"`: Polls standard UDP socket via `hml_reasm_feed_datagram()`.
* **Task C.2 (Three-Tier Memory Architecture & Auto-Fallback):**
  * Implement configurable `tier` parameter (`"auto"` [default], `"cpu"`, `"gpu_indirect"`, `"gpu_direct"`):
    * **Priority 1 / Tier 3 (GPU-direct):** Register GPU device pointer directly with verbs (`ibv_reg_mr` via `nvidia-peermem`). Hardware/driver DMAs directly into VRAM. Zero host bounce. Export as `kDLCUDA`.
    * **Priority 2 / Tier 2 (GPU-indirect):** Ingest into host RAM buffer. Perform `cudaMemcpyAsync` to GPU device memory on stream. Export as `holoscan::Tensor` with device `kDLCUDA`.
    * **Priority 3 / Tier 1 (CPU-only):** Ingest directly into host memory buffer. Export as `holoscan::Tensor` with device `kDLCPU`.
    * **Auto-Fallback (`"auto"`):** Hierarchical resolution: Attempts Priority 1 (GPU Direct via Verbs + peermem); if unavailable falls back to Priority 2 (GPU Indirect via CUDA); if CUDA is unavailable, falls back to Priority 3 (CPU Only).
    * **QP Service:** Reliable Connected (`RC`) mode is now the system-wide default transport service, with Unreliable Connected (`UC`) available as an override.
* **Task C.3 (DLPack TensorMap Packaging & Application Example):**
  * Files: `examples/holoscan_ai_pipeline/main.cpp`, `examples/holoscan_ai_pipeline/CMakeLists.txt`
  * Wrap memory pointer into standard `DLManagedTensorCtx` and emit `holoscan::TensorMap`.
  * Construct full end-to-end Holoscan application connecting `HmlReceiverOp` to a downstream `TensorRtInferenceOp`.

---

### Milestone 3: End-to-End System Integration & Verification Matrix

#### Track D: Validation Matrix & Benchmarking
* **Task D.1 (6-Way Integration Verification Suite):**
  * Automated testing script exercising all 6 configuration paths:
    1. T0 UDP $\rightarrow$ CPU-Only Tensor
    2. T0 UDP $\rightarrow$ GPU-Indirect Tensor
    3. T1a RoCEv2 (Soft-RoCE `rxe0`) $\rightarrow$ CPU-Only Tensor
    4. T1a RoCEv2 (Soft-RoCE `rxe0`) $\rightarrow$ GPU-Indirect Tensor
    5. T1a RoCEv2 (Soft-RoCE `rxe0`) $\rightarrow$ GPU-Direct Tensor (or graceful probe skip)
    6. Custom Socket Control Handshake (`SET_DEST` $\rightarrow$ `START` $\rightarrow$ `STOP` $\rightarrow$ `RESET`)
* **Task D.2 (Documentation & Quickstart Guide):**
  * Update `docs/holoscan_mcu_lite_complete_guide.html` and markdown guides with exact step-by-step commands for third-party developers.

---

## Proposed Changes (File-by-File Breakdown)

### Component 1: Server Verbs Engine & Soft-RoCE
#### [NEW] `host/verbs_rx/hml_verbs_rx.h`
Public API for host-side `libibverbs` QP management and polling.
#### [NEW] `host/verbs_rx/hml_verbs_rx.c`
Implementation of UC QP setup, memory registration, and CQ completion polling.
#### [NEW] `host/verbs_rx/CMakeLists.txt`
Build rules linking `libibverbs`.

### Component 2: MCU Transports & Control
#### [MODIFY] `src/transports/rocev2/hml_rocev2.c`
Add BTH Immediate Data OpCode `0x2B` and payload appending on EOF fragment.
#### [MODIFY] `src/control/hml_control.c`
Process dynamic destination updates for verbs queue pairs.

### Component 3: Holoscan Operator & AI Application
#### [NEW] `examples/holoscan_operator/hml_receiver_op.hpp`
C++ Holoscan Operator declaration supporting 3 tiers and 2 transports.
#### [NEW] `examples/holoscan_operator/hml_receiver_op.cpp`
Implementation of ingestion, CUDA stream synchronization, and DLPack emission.
#### [NEW] `examples/holoscan_ai_pipeline/main.cpp`
Complete Holoscan application composing `HmlReceiverOp` with stock downstream inference.
#### [NEW] `examples/holoscan_ai_pipeline/CMakeLists.txt`
CMake integration with Holoscan SDK and CUDA.

---

## Verification Plan

### Automated Regression & Unit Tests
```bash
# 1. Clean build of core library and host tools
cmake -S . -B build -DHML_WARNINGS_AS_ERRORS=ON -DHML_BUILD_EXAMPLES=ON
cmake --build build -j$(nproc)

# 2. Run full 62-test regression suite
ctest --test-dir build --output-on-failure
```

### Soft-RoCE Reference Lab Validation
```bash
# 1. Verify Soft-RoCE environment
cd host/soft_roce
sudo ./check_env.sh

# 2. Run automated veth loopback RoCE verification
sudo ./run_lab.sh --keep
```

### 6-Way Matrix Manual Verification
```bash
# Verify UDP -> CPU Ingestion
./build/examples/holoscan_ai_pipeline --transport udp --tier cpu &
./build/adapters/synthetic/hml_gen --port 55101 --frames 10 --size 4096

# Verify RoCEv2 -> GPU Indirect Ingestion via Soft-RoCE
./build/examples/holoscan_ai_pipeline --transport verbs --device rxe0 --tier gpu_indirect &
./build/adapters/synthetic/hml_gen --xport rocev2 --port 4791 --frames 10 --size 4096
```

---

## Implementation Status Summary (Complete)

All tracks and milestones have been fully implemented and verified:

- [x] **Track A (MCU Transports & Control):**
  - RoCEv2 RC and UC dual-mode support with OpCodes `0x0A` (RC Write), `0x0B` (RC Write w/ Imm), `0x2A` (UC Write), `0x2B` (UC Write w/ Imm).
  - 4-byte Immediate data signaling at offset 56 with payload at offset 60, ICRC over BTH..payload.
  - Setters `hml_roce_set_qp_type()` and `hml_roce_set_imm_mode()`.
  - MCU custom socket control device streaming over RoCEv2 / UDP with dynamic negotiation.
- [x] **Track B (Server Verbs & Soft-RoCE):**
  - `host/verbs_rx/hml_verbs_rx.c`: Full `libibverbs` lifecycle with support for `IBV_QPT_RC` and `IBV_QPT_UC`.
  - Non-blocking work completion polling `hml_verbs_poll_cq()` on Immediate data.
  - Soft-RoCE reference harness and test client (`hml_verbs_rx_test_client`).
- [x] **Track C (Holoscan Multi-Tier Custom Operator):**
  - `examples/holoscan_operator/hml_receiver_op.hpp` & `.cpp`: Multi-transport (`udp`, `verbs`) and 3 memory tiers (`cpu`, `gpu_indirect`, `gpu_direct`).
  - `examples/holoscan_operator/hml_dlpack.hpp`: DLPack managed tensor encapsulation for `holoscan::TensorMap`.
  - `examples/holoscan_ai_pipeline/main.cpp`: Complete Holoscan AI pipeline with CUDA stream synchronization and DLPack tensor consumption.
- [x] **Track D (Verification Matrix & Documentation):**
  - `tests/matrix/test_6way_matrix.sh`: Automated 6-way verification matrix integrated as Test #65 in CTest (100% pass).
  - Comprehensive documentation and interactive guides in `docs/architecture.md`, `README.md`, and `docs/holoscan_mcu_lite_complete_guide.html`.

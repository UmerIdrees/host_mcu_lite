# M3b -- Soft-RoCE (`rdma_rxe`) verbs lab

**Status: lab note only. No production performance claim of any kind is made
or implied by anything in this document.**

## 1. Purpose (and the limit of it)

This lab exists for one reason: to exercise the **ibverbs-level plumbing** --
`ibv_open_device`, PD/MR registration, QP bring-up, and CQ completion
delivery -- against a software RDMA NIC that any Linux box can create for
itself. `rdma_rxe` ("RXE", Soft-RoCE) is a kernel module that implements the
InfiniBand verbs API on top of an ordinary network device, so a single host can
host a two-sided lab with two namespaces, two veths and two rxe devices.

What this lab is **not**:

| | |
|---|---|
| Not a performance proxy | rxe executes in the kernel on a veth pair. It has no DMA engine, no hardware queue, no firmware. Any latency, bandwidth or jitter number produced here says nothing about a ConnectX NIC. |
| Not a prediction of GPU Direct RDMA | Whether GPU Direct works, and how fast, is a function of the GPU, the driver, GDRCOPY, PCIe topology and the NIC. rxe cannot address a GPU BAR at all. |
| Not the T1a data path | T1a (M3) is **socket-crafted RoCEv2** emitted by `src/transports/rocev2` and received by `host/roce_demux`. It never touches libibverbs. |
| Not an acceptance gate | The plan (`docs/holoscan_mcu_lite_Plan.md`, M3b) marks Soft-RoCE "optional, correctness only". A green lab here adds no coverage to the T1a claim; a skipped lab costs the project nothing. |

Expected divergence, called out in the plan: *"T1a socket RoCE works but
Soft-RoCE diverges -> keep M3b lab-only (expected)."* rxe and a ConnectX NIC
are different implementations of the same verb semantics, and rxe will happily
accept or reject things a NIC would not. Treat any rxe surprise as a question
about rxe until a real NIC says otherwise.

## 2. Prerequisites

Nothing here is needed to build, install or test `holoscan_mcu_lite`. The
library and its CTest suite must stay green on a machine with no RDMA at all.

To run the real lab you need:

1. **Linux with network namespaces** -- `CONFIG_NET_NS` (any distro kernel).
2. **Root** (`uid 0`), or `sudo` -- `ip netns`, `ip link add ... type veth`,
   `rdma link add ... type rxe` are all privileged.
3. **iproute2** -- `ip` (with netns support).
4. **rdma-core** -- the `rdma` CLI (`rdma link add ... type rxe`) and the
   ibverbs user-space tools `ibv_devinfo` (package name varies by distro:
   `libibverbs-utils`, `rdma-core`, or bundled with `infiniband-diags`).
   `ibstat` and `perftest` are optional.
5. **The `rdma_rxe` kernel module** -- `CONFIG_RDMA_RXE=m`. Usually shipped as
   `modules-extra` / `kernel-modules-extra` for your running kernel, or
   built out of tree from the `rxe` kernel drivers.
6. Optional: `iputils-ping` (connectivity check), `shellcheck` (lint).

Debian/Ubuntu sketch:

```sh
sudo apt-get install -y iproute2 rdma-core libibverbs-utils kmod \
                        linux-modules-extra-$(uname -r)
```

Fedora/RHEL sketch:

```sh
sudo dnf install -y iproute rdma-core libibverbs-utils kmod \
                     kernel-modules-extra-$(uname -r)
```

`check_env.sh` verifies all of this and names whatever is missing.

## 3. Files

| Path | Role |
|---|---|
| `host/soft_roce/lab_common.sh` | shared helpers; sourced, never executed; implements the exit-77 skip protocol |
| `host/soft_roce/check_env.sh` | read-only environment report; exit 0 = runnable, 77 = not |
| `host/soft_roce/setup_rxe.sh` | idempotently builds the two-namespace rxe lab |
| `host/soft_roce/teardown_rxe.sh` | removes it; safe to run twice |
| `host/soft_roce/run_lab.sh` | driver: report -> setup -> verify -> dump -> loopback pair -> teardown |
| `host/soft_roce/skip_selfcheck.sh` | CTest `roce_lab_skip_path`: proves every script fails closed |
| `host/soft_roce/artifacts/` | logs, git-ignored, created on demand |

## 4. Topology

```
 netns hml-a                          netns hml-b
 +-------------------------+         +-------------------------+
 | 10.200.0.1/24           |         |  10.200.0.2/24          |
 |   eth0 (veth-hml-a)     |=========|    eth0 (veth-hml-b)    |
 |     bound to rxe0       |  veth   |      bound to rxe1      |
 +-------------------------+         +-------------------------+
```

Names, addresses and MTU are overridable through the environment
(`HML_NS_A`, `HML_NS_B`, `HML_VETH_A`, `HML_VETH_B`, `HML_ETH_A`,
`HML_ETH_B`, `HML_RXE_A`, `HML_RXE_B`, `HML_IP_A`, `HML_IP_B`, `HML_CIDR`,
`HML_MTU`), so a second lab can coexist on one box.

## 5. Exact commands

```sh
cd host/soft_roce

# 1. Is the lab runnable here? (read-only)
./check_env.sh                      # 0 = runnable, 77 = not, 1 = report error
sudo ./check_env.sh                 # same, but as root

# 2. Everything at once, with teardown guaranteed by an EXIT trap
sudo ./run_lab.sh                   # add --keep to inspect the lab afterwards
./run_lab.sh --keep                 # keeps the namespaces up
sudo ./teardown_rxe.sh              # always safe, always idempotent

# 3. Force the skip path (no root, no RDMA, no modules) -- used by CTest
HML_RXE_SKIP=1 ./check_env.sh       # 77
HML_RXE_SKIP=1 ./run_lab.sh         # 77
```

Manual, step by step (what `setup_rxe.sh` does):

```sh
sudo ip netns add hml-a
sudo ip netns add hml-b
sudo ip link add veth-hml-a type veth peer name veth-hml-b
sudo ip link set veth-hml-a netns hml-a
sudo ip link set veth-hml-b netns hml-b
sudo ip -n hml-a link set veth-hml-a name eth0
sudo ip -n hml-b link set veth-hml-b name eth0
sudo ip -n hml-a addr add 10.200.0.1/24 dev eth0
sudo ip -n hml-b addr add 10.200.0.2/24 dev eth0
sudo ip -n hml-a link set eth0 mtu 1500 up
sudo ip -n hml-b link set eth0 mtu 1500 up
sudo ip -n hml-a link set lo up
sudo ip -n hml-b link set lo up
sudo modprobe rdma_rxe
sudo ip netns exec hml-a rdma link add rxe0 type rxe netdev eth0
sudo ip netns exec hml-b rdma link add rxe1 type rxe netdev eth0
sudo ip netns exec hml-a ibv_devinfo -d rxe0
sudo ip netns exec hml-b ibv_devinfo -d rxe1

# tear down
sudo ip netns del hml-a
sudo ip netns del hml-b     # also destroys their veth and rxe devices
```

CTest:

```sh
# the always-registered, always-passing skip-path test
ctest -R roce_lab_skip_path

# the real lab: opt in, then run as root
cmake -S . -B build -DHML_ENABLE_ROCE_LAB=ON
sudo -E cmake --build build && sudo -E ctest --test-dir build -R roce_lab
# or, without touching the cache:
HML_ENABLE_ROCE_LAB=1 cmake -S . -B build && sudo -E ctest --test-dir build -R roce_lab
```

`roce_lab` is **not** registered by default, so a plain `ctest` run never needs
root, RDMA or `rdma_rxe`. Verified: a default configure lists exactly one
`roce_lab*` test (`roce_lab_skip_path`, passing), and
`-DHML_ENABLE_ROCE_LAB=ON` (or `HML_ENABLE_ROCE_LAB=1` in the environment)
adds `roce_lab`, which reports `Skipped` -- never `Failed` -- on a box
without the lab.

Implementation note: the top-level `CMakeLists.txt` calls `enable_testing()`
*after* `add_subdirectory(host/soft_roce)`, so `host/soft_roce/CMakeLists.txt`
calls `enable_testing()` locally as well -- otherwise `add_test()` there would
be silently a no-op. Do not remove that line.

Lint: the scripts are POSIX-bash clean and shellcheck-clean. Lint them with
source following enabled, because `lab_common.sh` is sourced:

```sh
shellcheck -s bash -x host/soft_roce/*.sh      # -x is required (SC1091 otherwise)
```

## 6. Expected `ibv_devinfo` output shape

Shape only -- exact strings, GIDs, ports and `vendor_id` are host- and
version-dependent and are deliberately not pinned:

```
hml_a# ibv_devinfo -d rxe0
hca_id:   rxe0                          <- MUST be rxe0 inside netns hml-a
  transport:            InfiniBand (0)
  fw_ver:                0.0.0
  node_guid:             fe80:0000:0000:0000
  sys_image_guid:        fe80:0000:0000:0000
  vendor_id:             0x0000
  vendor_part_id:        0x0000
  hw_ver:                0x0
  ...
  phys_port_cnt:         1
    port:   1
      state:            PORT_ACTIVE (4)
      max_mtu:          1024 (Active)
      ...
      GID[  0]:          0/fe80::<hml-a eth0 link-local>   <- the GID rxe derives
      port_lid:          1
      port_lmc:          0x00
      link_layer:        Ethernet
      ...
  total_mr:              N        <- grows with registered MRs
```

Expectations `run_lab.sh` actually asserts:

* `ibv_devinfo -d rxe0` **exits 0** inside `netns hml-a`;
* its output **contains the string `rxe0`**;
* its output **contains a `GID` line** (else a warning, not a failure);
* the same for `rxe1` inside `netns hml-b`.

Two rxe specifics worth knowing before you chase a "failure":

* the GID is derived from the link-local IPv6 address of the bound netdev, so
  it changes every time the veth is recreated;
* if IPv6 is disabled in the namespace, the GID may print as
  `0/0000:0000:0000:0000:0000:0000:0000:0000`. That is still a valid rxe GID
  and still a working lab.

## 7. Skip behaviour (fail closed, never fail CI)

Every script in this directory fails closed with **exit 77** -- CTest's
"skipped" return code -- when a prerequisite is missing, and every skip
message names the missing item:

```
  [skip] required tool 'rdma' not found in PATH. Missing: 'rdma'. (rdma-core ...)
  [skip] M3b Soft-RoCE lab is OPTIONAL; failing closed, CI continues.
```

The skip triggers on: `HML_RXE_SKIP=1`; non-root; `ip`/`rdma`/`modprobe`
missing; `ip netns` unusable; `rdma_rxe` absent or unloadable; `ibv_devinfo`
missing. `roce_lab` carries `SKIP_RETURN_CODE 77`, so an under-provisioned
runner reports *Skipped*, never *Failed*.

Two tests exist:

* **`roce_lab_skip_path`** -- always registered, **always expected to pass**.
  It runs `skip_selfcheck.sh`, which invokes `check_env.sh`, `setup_rxe.sh`,
  `teardown_rxe.sh` and `run_lab.sh` with `HML_RXE_SKIP=1`, requires exit 77
  from each, and additionally asserts that the forced-skip path created no
  `artifacts/` directory. It needs no root, no RDMA and no network, and it
  fails loudly if the skip path ever stops working -- a lab that cannot fail
  closed is not trustworthy.
* **`roce_lab`** -- registered only with `-DHML_ENABLE_ROCE_LAB=ON` (or
  `HML_ENABLE_ROCE_LAB` in the environment). It needs root and `rdma_rxe`.

`artifacts/` holds the `ibv_devinfo`, `ibstat` and loopback logs from a real
run and is git-ignored via `host/soft_roce/.gitignore`.

## 8. M3 socket-RoCE loopback sanity pair

Step (e) of `run_lab.sh` looks for already-built M3 binaries in `../build`,
`../../build` and `/tmp/opencode/build*`:

* `hml_gen_roce` -- the T1a emitter CLI (socket-crafted RoCEv2);
* `hml_host_roce` -- the T1a receiver CLI (`host/roce_demux`).

If both exist, the pair is run over loopback (`127.0.0.1`) with the receiver's
documented defaults -- port **14791**, window_base **0x100000**, window_len
**65536**, frame_size **9000**, rkey **0xDEADBEEF** -- and the resulting logs
are saved to `artifacts/`. Note that 14791 is an unprivileged mirror of
`HML_ROCE_PORT` (4791); binding 4791 needs root or `CAP_NET_BIND_SERVICE`.

If the emitter CLI is not built, step (e) **skips with a message and the lab
still passes**. In-tree, M3 loopback coverage does not depend on this lab at
all: run `ctest -R roce`, which drives `tests/m3/test_roce_e2e` (the same
emitter into the same demux) plus the golden-wire and ICRC tests.

## 9. Two deliberate T1a wire deviations (re-validate in M6)

Decided elsewhere in M3, implemented in `src/transports/rocev2` and
`host/roce_demux`, and recorded here because Soft-RoCE is the only place where
they could be checked against a second implementation of the verbs:

**(a) BTH field positions follow the real InfiniBand layout.** The 12-byte Base
Transport Header is laid out as the IB spec defines it, not as a
project-invented order:

```
off 28  12  BTH    [0] OpCode 0x2A = UC (0b001 << 5) | RDMA (0x0A)
                  [1] flags  0x00 (RDMA Write, no immediate)
                  [2] reserved 0x00
                  [3..5] PSN, 24-bit, big-endian
                  [6..7] AETH 0x0000
                  [8..11] reserved 0x00000000
off 40  16  RETH   [0..7]  virtual address, big-endian u64
                  [8..11] rkey, big-endian u32
                  [12..15] length, big-endian u32, D bit = 0
off 56  len  RDMA Write payload
off 56+len 4  ICRC  CRC-32/IEEE over [BTH .. end of payload], stored LITTLE-endian
```

Total 60 + len bytes with len <= 1440, so a datagram never fragments at a
1500-byte MTU. The demux accepts the UC `0x2A` and the RC `0x0A` opcodes.

**(b) The frame-end metadata record is 20 bytes, not 16.** T1a has no in-band
EOF, so a frame is declared complete by a single RDMA WRITE to a dedicated
*meta slot* at `dest.remote_addr + dest.frame_size`. That slot carries a
20-byte little-endian record (`HML_ROCE_META_LEN = 20`):

```
[0..3]   u32 magic          [4..5] u16 stream_id   [6..7] u16 flags
[8..11]  u32 frame seq     [12..15] u32 frame bytes
[16..19] u32 CRC-32/IEEE of the frame payload
```

4 + 2 + 2 + 4 + 4 + 4 = 20. The "16 bytes" figure in the early M3 notes was a
miscount; the explicit field list is authoritative. The receiver delivers a
frame to its callback only when the CRC matches.

**Both deviations are unverified against real hardware.** rxe does not help
here: a verb-level client on a ConnectX NIC is driven by the NIC's own
firmware-visible BTH handling, so the deviations that matter are the ones a
NIC-side capture would show -- field offsets, PSN width/endianness, RETH
ordering, ICRC coverage and placement, and the actual RoCEv2 outer-header
shape (`HML_ROCE_IP_HDR_LEN` 20 + `HML_ROCE_UDP_HDR_LEN` 8, UDP checksum 0,
`HML_ROCE_PORT` 4791). **Re-validate both (a) and (b) against a ConnectX
packet capture in M6, before any claim about interoperability with real
hardware.** Until that capture exists, T1a is "correct by construction and by
golden-vector test", not "confirmed on a NIC".

## 10. What a user must do to run the real lab

```sh
sudo apt-get install -y iproute2 rdma-core libibverbs-utils kmod \
                        linux-modules-extra-$(uname -r)   # or the dnf equivalent
sudo modprobe rdma_rxe
cd host/soft_roce
sudo ./run_lab.sh            # report -> setup -> verify -> dump -> teardown
```

Exit 0 means the lab came up and the devices were verified. The log files
under `artifacts/` are the evidence. **Read them as plumbing evidence only:
Soft-RoCE results do not predict ConnectX or GPU Direct performance.**

## 11. Renode Emulated MCU to Soft-RoCE TAP Bridging

For end-to-end microcontroller emulation where the emulated PHY wire streams packets directly into the host's Linux Kernel Soft-RoCE subsystem:

1. **Host Setup:** Run [`examples/renode_soft_roce_e2e/setup_tap_soft_roce.sh`](file:///home/vboxuser/Desktop/work/work_04_holoscan_mcu_lite/examples/renode_soft_roce_e2e/setup_tap_soft_roce.sh):
   ```sh
   sudo ./examples/renode_soft_roce_e2e/setup_tap_soft_roce.sh
   ```
   This loads `rdma_rxe`, provisions `tap0` with `10.0.0.2/24`, binds `rxe0` to `tap0`, and assigns ownership to the unprivileged user.

2. **Renode Virtual Machine:** Launch [`renode/scripts/stm32f767_rxe.resc`](file:///home/vboxuser/Desktop/work/work_04_holoscan_mcu_lite/renode/scripts/stm32f767_rxe.resc):
   The Cortex-M7's Synopsys Ethernet MAC (`02:00:00:00:00:01`, `10.0.0.1`) attaches to the host TAP device. Once `/dev/net/tun` is opened, `rxe0` transitions from `POLLING` to `PORT_ACTIVE (4)`.

3. **Master Ingestion & Benchmark:** Run [`examples/renode_soft_roce_e2e/test_all_control_data_high_speed.sh`](file:///home/vboxuser/Desktop/work/work_04_holoscan_mcu_lite/examples/renode_soft_roce_e2e/test_all_control_data_high_speed.sh) to verify all 11 control opcodes, high-speed T0 UDP streaming (zero-drop), high-speed T1a RoCEv2 (zero ICRC errors / zero PSN gaps), and Holoscan AI pipeline custom operator ingestion.

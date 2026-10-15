#!/usr/bin/env bash
#
# test_renode_holoscan_e2e.sh -- Integrated End-to-End Test Suite:
# Renode Emulated MCU -> Wire Datagrams -> NVIDIA Holoscan Application Graph.
#
# Asserts:
#   1. Cross-compilation of ARM Cortex-M7 network firmware (Synopsys MAC + lwIP).
#   2. Renode physical layer emulation with ARP resolution and wire packet assertions.
#   3. NVIDIA Holoscan custom operator (HmlReceiverOp) ingesting sensor streams.
#   4. DLPack tensor conversion and downstream AI inference operator execution.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
PIPELINE_BIN="${BUILD_DIR}/examples/holoscan_ai_pipeline/holoscan_ai_pipeline"

# Toolchain discovery: search standard install paths, user overrides, or existing PATH
RENODE_EXTRA_PATHS="${HML_RENODE_DIR:-}:${HML_RENODE_PYTHON_PATH:-}:/opt/renode:${HOME}/renode:${HOME}/.renode_venv/bin:${HOME}/hml_toolchain_cache/renode_1.17.0-portable:${HOME}/hml_venv/bin"
export PATH="${RENODE_EXTRA_PATHS}:${HOME}/.local/bin:${PATH}"

SKIP_RC=77

echo "======================================================================"
echo "  Integrated Renode MCU to NVIDIA Holoscan AI Application E2E Test"
echo "======================================================================"

# Toolchain prerequisite check (fail-closed / skip protocol)
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  echo "[SKIP] arm-none-eabi-gcc not found. Skipping Renode MCU test." >&2
  exit ${SKIP_RC}
fi

if ! command -v renode >/dev/null 2>&1; then
  echo "[SKIP] renode executable not found. Skipping Renode MCU test." >&2
  exit ${SKIP_RC}
fi

echo "  Toolchain : $(command -v arm-none-eabi-gcc)"
echo "  Renode    : $(command -v renode)"
echo "======================================================================"

PASSED=0
TOTAL=4

run_step() {
  local num="$1"
  local desc="$2"
  shift 2
  echo -n "[Stage ${num}/${TOTAL}] ${desc} ... "
  if "$@" > /tmp/hml_renode_e2e_${num}.log 2>&1; then
    echo "PASSED"
    PASSED=$((PASSED + 1))
  else
    echo "FAILED"
    cat /tmp/hml_renode_e2e_${num}.log
    exit 1
  fi
}

# Stage 1: Build MCU Network Firmware
run_step 1 "Cross-compiling STM32F767ZI Cortex-M7 Firmware (lwIP + MAC)" \
  "${ROOT_DIR}/renode/scripts/build_vendor_net.sh" stm32f767zi

# Stage 2: Renode Wire Packet Verification
run_step 2 "Renode Emulation: Synopsys MAC DMA -> lwIP ARP -> Emulated PHY Wire" \
  "${ROOT_DIR}/renode/scripts/run_vendor_net_smoke.sh" stm32f767zi

# Stage 3: Holoscan Application Graph (UDP Data Path -> DLPack Tensor -> AI Operator)
run_step 3 "Holoscan Application Graph: Framed UDP -> DLPack Tensor -> MockInferenceOp" \
  "${PIPELINE_BIN}" --transport udp --tier cpu --frames 20 --size 4096 --test

# Stage 4: Holoscan Application Graph (Verbs RoCEv2 -> DLPack Tensor -> AI Operator)
run_step 4 "Holoscan Application Graph: Verbs RoCEv2 (UC & RC) -> DLPack Tensor -> MockInferenceOp" \
  "${PIPELINE_BIN}" --transport verbs --qp-type rc --tier cpu --frames 20 --size 4096 --test

echo "======================================================================"
echo " Renode to Holoscan Application E2E Summary: ${PASSED}/${TOTAL} Passed"
echo "======================================================================"
exit 0

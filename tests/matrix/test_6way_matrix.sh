#!/usr/bin/env bash
#
# test_6way_matrix.sh -- End-to-End System Integration Verification Matrix
#
# Validates the 6 ingestion pathways defined in holoscan_multi_tier_bridge_plan.md:
#   1. T0 UDP -> CPU-Only DLPack Tensor
#   2. T0 UDP -> GPU-Indirect DLPack Tensor
#   3. T1a RoCEv2 (RC Queue Pair) -> CPU-Only DLPack Tensor
#   4. T1a RoCEv2 (RC Queue Pair) -> GPU-Indirect DLPack Tensor
#   5. T1a RoCEv2 (RC Queue Pair) -> GPU-Direct DLPack Tensor (with graceful probe fallback)
#   6. Custom Socket Control Handshake (SET_DEST -> START -> STOP -> RESET)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
PIPELINE_BIN="${BUILD_DIR}/examples/holoscan_ai_pipeline/holoscan_ai_pipeline"
CTRL_DEV_BIN="${BUILD_DIR}/src/control/hml_ctrl_device"
CTRL_HOST_BIN="${BUILD_DIR}/src/control/hml_ctrl_host"

PASSED=0
FAILED=0
TOTAL=6

echo "======================================================================"
echo "    NVIDIA Holoscan Multi-Tier Ingestion Bridge: 6-Way Matrix Test"
echo "======================================================================"

run_test() {
  local num="$1"
  local name="$2"
  shift 2
  echo -n "[Test ${num}/${TOTAL}] ${name} ... "
  if "$@" > /tmp/hml_matrix_test_${num}.log 2>&1; then
    echo "PASSED"
    PASSED=$((PASSED + 1))
  else
    echo "FAILED"
    cat /tmp/hml_matrix_test_${num}.log
    FAILED=$((FAILED + 1))
  fi
}

# --- Path 1: T0 UDP -> CPU-Only Tensor ---
run_test 1 "T0 UDP -> Tier 1 (CPU-Only Tensor)" \
  "${PIPELINE_BIN}" --transport udp --tier cpu --frames 10 --size 4096 --test

# --- Path 2: T0 UDP -> GPU-Indirect Tensor ---
run_test 2 "T0 UDP -> Tier 2 (GPU-Indirect Tensor)" \
  "${PIPELINE_BIN}" --transport udp --tier gpu_indirect --frames 10 --size 4096 --test

# --- Path 3: T1a RoCEv2 (RC) -> CPU-Only Tensor ---
run_test 3 "T1a RoCEv2 (RC) -> Tier 1 (CPU-Only Tensor)" \
  "${PIPELINE_BIN}" --transport verbs --qp-type rc --tier cpu --frames 10 --size 4096 --test

# --- Path 4: T1a RoCEv2 (RC) -> GPU-Indirect Tensor ---
run_test 4 "T1a RoCEv2 (RC) -> Tier 2 (GPU-Indirect Tensor)" \
  "${PIPELINE_BIN}" --transport verbs --qp-type rc --tier gpu_indirect --frames 10 --size 4096 --test

# --- Path 5: T1a RoCEv2 (RC) -> GPU-Direct Tensor (Probe + Fallback) ---
run_test 5 "T1a RoCEv2 (RC) -> Tier 3 (GPU-Direct Tensor / Probe)" \
  "${PIPELINE_BIN}" --transport verbs --qp-type rc --tier gpu_direct --frames 10 --size 4096 --test

# --- Path 6: Custom Socket Control Handshake ---
test_control_handshake() {
  local dev_port=55188
  local reply_port=45588
  "${CTRL_DEV_BIN}" --ctrl-port "${dev_port}" --reply-port "${reply_port}" --frames 5 > /tmp/hml_ctrl_dev.log 2>&1 &
  local dev_pid=$!
  sleep 0.4
  
  local status=0
  "${CTRL_HOST_BIN}" --ctrl-port "${dev_port}" --local-port "${reply_port}" \
    --roce --qp 0x100 --rkey 0xdeadbeef --remote-addr 0x100000 --frame-size 4096 \
    --script "ping,set_dest,get_dest,start,status,stop,reset" > /tmp/hml_ctrl_host.log 2>&1 || status=$?

  kill -INT "${dev_pid}" 2>/dev/null || true
  wait "${dev_pid}" 2>/dev/null || true
  return "${status}"
}

run_test 6 "Custom Socket Control Handshake (SET_DEST -> START -> STOP -> RESET)" \
  test_control_handshake

echo "======================================================================"
echo " 6-Way Matrix Execution Summary: ${PASSED}/${TOTAL} Passed (${FAILED} Failed)"
echo "======================================================================"

if [ "${FAILED}" -eq 0 ]; then
  exit 0
else
  exit 1
fi

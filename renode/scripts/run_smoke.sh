#!/usr/bin/env bash
#
# run_smoke.sh -- run the Cortex-M smoke test under Renode.
#
# Exit codes (all of them are load-bearing):
#   0   the emulator ran and the firmware printed result=PASS
#   77  SKIPPED: Renode or the ARM toolchain is missing. CTest maps 77 to
#       "Skipped" (SKIP_RETURN_CODE). A machine with no emulator is never a red
#       build -- and, just as importantly, never a green one either.
#   2   usage error: a bad argument. A caller bug, never a skip.
#   1   the tooling was present and the run FAILED. Loud, on purpose: this is
#       the case that must never be softened into a skip.
#
# WHAT A PASS MEANS, AND WHAT IT DOES NOT
#   A pass means: the demo image booted on an emulated Cortex-M4, executed the
#   shipping holoscan_mcu_lite codec/CRC/reassembly code, and printed
#   HML-MCU-DONE... result=PASS on the UART.
#   A pass does NOT mean: FreeRTOS works, lwIP works, a datagram crosses a
#   network, a sensor works, or any real MCU was involved. The image's RTOS is
#   a cooperative stub and it has no network stack at all. See docs/renode.md.
#
# HML_MCU_SMOKE_SKIP=1 forces the skip path, which is how the CTest self-check
# proves the skip works on a box that happens to have Renode installed.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
RENODE_DIR_LOCAL=$(cd "$SCRIPT_DIR/.." && pwd)

# CTest's "skipped" return code.
HML_SKIP_RC=77

: "${HML_RENODE:=}"
: "${HML_RENODE_TEST:=}"
: "${HML_CROSS_PREFIX:=arm-none-eabi-}"
: "${HML_CROSS_CC:=${HML_CROSS_PREFIX}gcc}"
: "${HML_MCU_OUT:=${RENODE_DIR_LOCAL}/build}"
: "${HML_MCU_TIMEOUT:=300}"
: "${HML_MCU_BUILD_TIMEOUT:=180}"
: "${HML_MCU_SMOKE_SKIP:=0}"
: "${HML_MCU_KEEP_LOG:=0}"
# renode-test is a python script that imports robotframework, psutil and pyyaml.
# It is run with the system python3 unless these directories are prepended to
# PATH, so a venv that carries them is a one-line configuration, not a rebuild.
: "${HML_RENODE_PYTHON_PATH:=}"

usage() {
  cat <<'EOF'
usage: run_smoke.sh [--renode PATH] [--renode-test PATH] [--elf PATH]
                    [--timeout SEC] [--frame-bytes N] [--keep-log] [--help]

  --renode PATH       the renode executable (default: from $HML_RENODE, else PATH)
  --renode-test PATH  the renode-test wrapper (default: from $HML_RENODE_TEST,
                      else next to renode, else PATH)
  --elf PATH          use an already-built image instead of invoking build_mcu.sh
  --timeout SEC       wall-clock limit for the whole emulator run (default 300)
  --frame-bytes N     forwarded to build_mcu.sh (see its --help)
  --keep-log          keep renode/build/*.log after the run

Exit: 0 pass, 77 skipped (no Renode / no ARM toolchain), 2 usage error,
      1 the run failed.
env:   HML_RENODE, HML_RENODE_TEST, HML_CROSS_PREFIX, HML_CROSS_CC,
       HML_MCU_OUT, HML_MCU_TIMEOUT, HML_MCU_BUILD_TIMEOUT, HML_MCU_KEEP_LOG,
       HML_RENODE_PYTHON_PATH (directories prepended to PATH for renode-test),
       HML_MCU_SMOKE_SKIP=1 forces the skip path for CI self-testing.
EOF
}

skip() {
  printf 'run_smoke.sh: [skip] %s\n' "$1" >&2
  printf 'run_smoke.sh: [skip] The Cortex-M smoke test did NOT run. This is not a pass:\n' >&2
  printf 'run_smoke.sh: [skip] no Cortex-M code was executed and nothing was validated.\n' >&2
  printf 'run_smoke.sh: [skip] Install per docs/renode.md section 1 to run it for real.\n' >&2
  exit "$HML_SKIP_RC"
}

die() {
  printf 'run_smoke.sh: [fail] %s\n' "$1" >&2
  exit 1
}

usage_die() {
  printf 'run_smoke.sh: %s\n' "$1" >&2
  usage >&2
  exit 2
}

# ---- argument validation ----------------------------------------------------
ELF_OVERRIDE=""
FRAME_BYTES_ARG=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --renode)
      [ "$#" -ge 2 ] || usage_die "--renode needs a value"
      HML_RENODE="$2"
      shift 2
      ;;
    --renode-test)
      [ "$#" -ge 2 ] || usage_die "--renode-test needs a value"
      HML_RENODE_TEST="$2"
      shift 2
      ;;
    --elf)
      [ "$#" -ge 2 ] || usage_die "--elf needs a value"
      ELF_OVERRIDE="$2"
      shift 2
      ;;
    --timeout)
      [ "$#" -ge 2 ] || usage_die "--timeout needs a value"
      HML_MCU_TIMEOUT="$2"
      shift 2
      ;;
    --frame-bytes)
      [ "$#" -ge 2 ] || usage_die "--frame-bytes needs a value"
      FRAME_BYTES_ARG="--frame-bytes"
      FRAME_BYTES_VALUE="$2"
      shift 2
      ;;
    --keep-log)
      HML_MCU_KEEP_LOG=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      usage_die "unknown argument: $1"
      ;;
  esac
done

case "$HML_MCU_TIMEOUT" in
  ''|*[!0-9]*) usage_die "--timeout must be a positive integer, got '$HML_MCU_TIMEOUT'" ;;
esac
[ "$HML_MCU_TIMEOUT" -ge 5 ] || usage_die "--timeout must be at least 5 seconds"
if [ -n "$FRAME_BYTES_ARG" ]; then
  case "$FRAME_BYTES_VALUE" in
    ''|*[!0-9]*) usage_die "--frame-bytes must be a non-negative integer" ;;
  esac
fi

command -v timeout >/dev/null 2>&1 || die "'timeout' is not on PATH; refusing to run an unbounded command"

# ---- forced skip, used by the CTest self-check ------------------------------
if [ "$HML_MCU_SMOKE_SKIP" = "1" ]; then
  skip "HML_MCU_SMOKE_SKIP=1 is set (forced by the CTest self-check)"
fi

# ---- toolchain probe: renode, renode-test, and the cross compiler ----------
if [ -z "$HML_RENODE" ]; then
  HML_RENODE=$(command -v renode 2>/dev/null || true)
fi
if [ -z "$HML_RENODE" ]; then
  skip "the 'renode' executable was not found (looked in \$HML_RENODE and PATH). Install Renode from https://github.com/renode/renode/releases (a portable build needs mono on Linux or dotnet on newer builds), or set \$HML_RENODE=/path/to/renode. See docs/renode.md section 1."
fi
[ -x "$HML_RENODE" ] || die "HML_RENODE=$HML_RENODE is not executable"

if [ -z "$HML_RENODE_TEST" ]; then
  HML_RENODE_TEST=$(dirname "$HML_RENODE")/renode-test
  [ -x "$HML_RENODE_TEST" ] || HML_RENODE_TEST=$(command -v renode-test 2>/dev/null || true)
fi
[ -n "$HML_RENODE_TEST" ] || skip "renode was found at $HML_RENODE but no 'renode-test' wrapper was found next to it or on PATH. renode-test is the Robot Framework entry point this harness drives; point \$HML_RENODE_TEST at it, or run the .robot file by hand from the Renode IDE. See docs/renode.md section 2."
[ -x "$HML_RENODE_TEST" ] || die "HML_RENODE_TEST=$HML_RENODE_TEST is not executable"

# renode-test is a python3 program and its first import is `robot`. Without the
# Robot Framework it exits 1 with an ImportError, which would read as a failed
# emulator run. That is not what is happening, so it is a skip -- and it says so.
if ! PATH="$HML_RENODE_PYTHON_PATH:$PATH" python3 -c 'import robot, psutil, yaml' >/dev/null 2>&1; then
  skip "python3 on PATH (plus \$HML_RENODE_PYTHON_PATH='$HML_RENODE_PYTHON_PATH') cannot 'import robot, psutil, yaml', which is what renode-test needs to run a .robot file. Install them into the interpreter that runs renode-test, or point \$HML_RENODE_PYTHON_PATH at a venv that has them. See docs/renode.md section 2."
fi

if ! command -v "$HML_CROSS_CC" >/dev/null 2>&1 && [ -z "$ELF_OVERRIDE" ]; then
  skip "the ARM cross compiler '$HML_CROSS_CC' is not on PATH (override with HML_CROSS_CC) and no --elf was supplied, so the Cortex-M image cannot be built. Install gcc-arm-none-eabi (Debian/Ubuntu: sudo apt-get install gcc-arm-none-eabi; Fedora: sudo dnf install arm-none-eabi-gcc-cs; macOS: brew install --cask gcc-arm-embedded). See docs/renode.md section 1."
fi

printf 'run_smoke.sh: renode       : %s\n' "$HML_RENODE"
printf 'run_smoke.sh: renode-test  : %s\n' "$HML_RENODE_TEST"
printf 'run_smoke.sh: python path  : %s\n' "${HML_RENODE_PYTHON_PATH:-<unchanged>}"
printf 'run_smoke.sh: timeout      : %ss\n' "$HML_MCU_TIMEOUT"

# ---- build the image (or accept the one the caller supplied) ----------------
mkdir -p "$HML_MCU_OUT"
ELF="$ELF_OVERRIDE"
if [ -z "$ELF" ]; then
  printf 'run_smoke.sh: building the Cortex-M image (stub RTOS, no lwIP)...\n'
  set +e
  # shellcheck disable=SC2086  # FRAME_BYTES_ARG/VALUE are optional word-split args
  timeout "$HML_MCU_BUILD_TIMEOUT" "$SCRIPT_DIR/build_mcu.sh" --out "$HML_MCU_OUT" \
    $FRAME_BYTES_ARG ${FRAME_BYTES_VALUE:+"$FRAME_BYTES_VALUE"}
  build_rc=$?
  set -e
  if [ "$build_rc" = "124" ]; then
    die "build_mcu.sh timed out after ${HML_MCU_BUILD_TIMEOUT}s"
  fi
  if [ "$build_rc" = "$HML_SKIP_RC" ]; then
    skip "build_mcu.sh reported the ARM toolchain is unavailable (exit 77)"
  fi
  [ "$build_rc" = "0" ] || die "build_mcu.sh failed (exit $build_rc)"
  ELF="$HML_MCU_OUT/hml_mcu_demo.elf"
fi

[ -f "$ELF" ] || die "the firmware image is missing: $ELF"
printf 'run_smoke.sh: image        : %s (%s bytes)\n' "$ELF" "$(wc -c <"$ELF" | tr -d ' ')"

# ---- run the robot test -----------------------------------------------------
# renode-test 1.17 has no --console/--hide-results (they died with the old
# runner), and its wrapper ALWAYS passes "-r $(pwd)", so the Robot results,
# the per-test logs/ and the failure snapshots/ all land in the current working
# directory. Run it from a scratch directory of its own so the repository never
# collects emulator droppings.
LOG_DIR="$HML_MCU_OUT"
ROBOT_LOG="$LOG_DIR/renode_smoke.log"
RESULTS_DIR="$LOG_DIR/renode_results"
rm -f "$ROBOT_LOG"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

printf 'run_smoke.sh: running renode-test (max %ss)...\n' "$HML_MCU_TIMEOUT"
set +e
(
  cd "$RESULTS_DIR" || exit 125
  PATH="$HML_RENODE_PYTHON_PATH:$PATH"
  export PATH
  timeout --signal=TERM --kill-after=15 "$HML_MCU_TIMEOUT" \
    "$HML_RENODE_TEST" \
      --show-log \
      --save-logs=always \
      --variable "ELF:$ELF" \
      "$RENODE_DIR_LOCAL/holoscan_mcu_lite.robot"
) >"$ROBOT_LOG" 2>&1
rc=$?
set -e
# The UART markers live in the Renode log the terminal tester wrote, not in the
# Robot summary, so fold them into $ROBOT_LOG: without this the printed evidence
# would be a PASS and nothing else.
if [ -d "$RESULTS_DIR/logs" ]; then
  {
    printf '\nrun_smoke.sh: ---- UART output captured by the terminal tester ----\n'
    cat "$RESULTS_DIR"/logs/*.log 2>/dev/null || true
  } >>"$ROBOT_LOG"
fi

case "$rc" in
  0)
    printf '\nrun_smoke.sh: ---- renode-test output ----\n'
    sed 's/^/  /' "$ROBOT_LOG"
    printf 'run_smoke.sh: PASS -- the emulated Cortex-M printed result=PASS.\n'
    printf 'run_smoke.sh: what that proves: this C code runs on an emulated Cortex-M4\n'
    printf 'run_smoke.sh: and the codec/CRC/reassembly checks passed inside the image.\n'
    printf 'run_smoke.sh: what it does NOT prove: FreeRTOS (the RTOS is a cooperative\n'
    printf 'run_smoke.sh: stub), lwIP (absent), any datagram on any network, any sensor,\n'
    printf 'run_smoke.sh: and any real silicon. Emulated Cortex-M is not an MCU.\n'
    if [ "$HML_MCU_KEEP_LOG" = "1" ]; then
      printf 'run_smoke.sh: log kept at %s\n' "$ROBOT_LOG"
    else
      rm -f "$ROBOT_LOG"
    fi
    exit 0
    ;;
  124)
    printf 'run_smoke.sh: ---- renode-test output (tail) ----\n' >&2
    tail -n 40 "$ROBOT_LOG" | sed 's/^/  /' >&2
    printf 'run_smoke.sh: [fail] renode-test exceeded %ss and was killed.\n' "$HML_MCU_TIMEOUT" >&2
    printf 'run_smoke.sh: [fail] A hang is a FAILURE, never a skip: the image ran but the\n' >&2
    printf 'run_smoke.sh: [fail] assertions never completed. Full log: %s\n' "$ROBOT_LOG" >&2
    exit 1
    ;;
  *)
    printf 'run_smoke.sh: ---- renode-test output ----\n' >&2
    sed 's/^/  /' "$ROBOT_LOG" >&2
    printf 'run_smoke.sh: [fail] renode-test exited %s. The tooling was present, so this\n' "$rc" >&2
    printf 'run_smoke.sh: [fail] is a real failure and is deliberately NOT converted into\n' >&2
    printf 'run_smoke.sh: [fail] a skip. Full log: %s\n' "$ROBOT_LOG" >&2
    exit 1
    ;;
esac

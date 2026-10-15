#!/usr/bin/env bash
#
# run_vendor_smoke.sh -- build and run one vendor platform's smoke test.
#
# Exit codes (all of them are load-bearing):
#   0   the emulator ran and the firmware printed result=PASS
#   77  SKIPPED: Renode or the vendor's cross toolchain is missing. CTest maps
#       77 to "Skipped" (SKIP_RETURN_CODE). A machine with no emulator is never
#       a red build -- and, just as importantly, never a green one either.
#   2   usage error: a bad argument. A caller bug, never a skip.
#   1   the tooling was present and the run FAILED. Loud, on purpose: this is
#       the case that must never be softened into a skip.
#
# WHAT A PASS MEANS, AND WHAT IT DOES NOT
#   A pass means: the vendor's demo image booted on its emulated machine,
#   executed the shipping holoscan_mcu_lite codec/CRC/reassembly code, and
#   printed HML-MCU-DEMO-DONE ... result=PASS on the UART.
#   A pass does NOT mean: FreeRTOS works, lwIP works, a datagram crosses a
#   network, a sensor works, or any real MCU was involved. The image's RTOS is
#   a cooperative stub and it has no network stack at all. On the RISC-V row
#   the stub itself lives in the vendor's startup file and is a shim, not a
#   port. See docs/renode.md and docs/renode_multi_vendor.md.
#
# THE MACHINE VARIABLE
#   renode-test takes variables, not a --machine switch, so this script accepts
#   --machine/--vendor as a script-level option and forwards it as
#   --variable MACHINE:<vendor>. The vendor's .robot derives its .resc name
#   from ${MACHINE}, and the ELF is passed explicitly. Any renode-test version
#   can drive any one of the five platforms this way.
#
# NEITHER renode-test NOR THE .robot HAS EVER RUN
#   On the machine this matrix was written on this script always skips, because
#   none of the tooling exists. A skip here is not a pass: it means nothing was
#   executed. See the STATUS blocks in renode/vendors/*/README.md.
#
# HML_MCU_SMOKE_SKIP=1 forces the skip path, which is how the CTest self-check
# proves the skip works on a box that happens to have Renode installed.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
RENO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

# CTest's "skipped" return code.
HML_SKIP_RC=77

: "${HML_RENODE:=}"
: "${HML_RENODE_TEST:=}"
: "${HML_CROSS_PREFIX:=}"
: "${HML_CROSS_CC:=}"
: "${HML_MCU_OUT:=${RENO_DIR}/build/%v}"
# Remember whether the caller pinned a timeout BEFORE the default is applied, so
# the per-vendor floor below can raise a bare default without ever overriding a
# value the user or CTest chose on purpose.
HML_MCU_TIMEOUT_WAS_SET="${HML_MCU_TIMEOUT:+yes}"
# The default is 900 s, and it is 900 s because 300 s was demonstrably too tight
# on a slow host rather than because anything needed more room. Measured PASSING
# runtimes for the rows that were failing to fit inside 300 s on this machine:
#
#   stm32f4   288.9 s     ti_tm4c  ~290 s     stm32f767zi  293.4 s
#
# A 300 s kill that lands 10 s short of a green run reports a HANG for a test
# that was about to pass, which is the most expensive kind of wrong. 900 s keeps
# over 3x headroom over the slowest measured row, and it costs nothing when a
# vendor really is broken: the .robot files bound the early lines at 30-120 s
# each, so a failure near the front still surfaces in seconds, as a named robot
# failure, long before this limit is reached.
: "${HML_MCU_TIMEOUT:=900}"
: "${HML_MCU_BUILD_TIMEOUT:=180}"
: "${HML_MCU_SMOKE_SKIP:=0}"
: "${HML_MCU_KEEP_LOG:=0}"
# renode-test is a python script that imports robotframework, psutil and pyyaml.
# It is run with the system python3 unless these directories are prepended to
# PATH, so a venv that carries them is a one-line configuration, not a rebuild.
: "${HML_RENODE_PYTHON_PATH:=}"

usage() {
  cat <<'EOF'
usage: run_vendor_smoke.sh VENDOR [--machine NAME] [--renode PATH]
                           [--renode-test PATH] [--elf PATH]
                           [--timeout SEC] [--frame-bytes N] [--keep-log] [--help]

  VENDOR          a vendor directory under renode/vendors/ (stm32f4, nxp_lpc,
                  ti_tm4c, microchip_sam, riscv). May also be given via
                  --machine NAME. Exactly one of the two must be given.
  --machine NAME  the machine name forwarded to renode-test as the MACHINE
                  variable; defaults to VENDOR.
  --renode PATH       the renode executable (default: from $HML_RENODE, else PATH)
  --renode-test PATH  the renode-test wrapper (default: from $HML_RENODE_TEST,
                      else next to renode, else PATH)
  --elf PATH          use an already-built image instead of invoking build_vendor.sh
  --timeout SEC       wall-clock limit for the whole emulator run (default 900;
                            raised automatically to 1200 for stm32f767zi, whose
                            64 KiB frame test legitimately needs ~375s)
  --frame-bytes N     forwarded to build_vendor.sh (see its --help)
  --keep-log          keep renode/build/<vendor>/*.log after the run

Exit: 0 pass, 77 skipped (no Renode / no cross toolchain), 2 usage error,
      1 the run failed.
env:   HML_RENODE, HML_RENODE_TEST, HML_RENODE_PYTHON_PATH, HML_CROSS_PREFIX,
       HML_CROSS_CC, HML_MCU_OUT (use %v for the vendor), HML_MCU_TIMEOUT,
       HML_MCU_BUILD_TIMEOUT, HML_MCU_KEEP_LOG,
       HML_MCU_SMOKE_SKIP=1 forces the skip path for CI self-testing.
EOF
}

usage_die() {
  printf 'run_vendor_smoke.sh: %s\n' "$1" >&2
  usage >&2
  exit 2
}

skip() {
  printf 'run_vendor_smoke.sh: [skip] %s\n' "$1" >&2
  printf 'run_vendor_smoke.sh: [skip] the %s smoke test did NOT run. This is not a pass:\n' "$VENDOR" >&2
  printf 'run_vendor_smoke.sh: [skip] no %s code was executed and nothing was validated.\n' \
    "$([ "$VENDOR" = "riscv" ] && echo "RV32" || echo "Cortex-M")" >&2
  printf 'run_vendor_smoke.sh: [skip] Install per docs/renode_multi_vendor.md section 3 to run it for real.\n' >&2
  exit "$HML_SKIP_RC"
}

die() {
  printf 'run_vendor_smoke.sh: [fail] %s\n' "$1" >&2
  exit 1
}

# ---- argument validation --------------------------------------------------
VENDOR=""
MACHINE=""
ELF_OVERRIDE=""
FRAME_BYTES_ARG=""
FRAME_BYTES_VALUE=""
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
    --machine)
      [ "$#" -ge 2 ] || usage_die "--machine needs a value"
      MACHINE="$2"
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
    -*)
      usage_die "unknown argument: $1"
      ;;
    *)
      [ -z "$VENDOR" ] || usage_die "vendor given twice: $VENDOR and $1"
      VENDOR="$1"
      shift
      ;;
  esac
done

if [ -n "$VENDOR" ] && [ -n "$MACHINE" ] && [ "$VENDOR" != "$MACHINE" ]; then
  usage_die "VENDOR ($VENDOR) and --machine ($MACHINE) disagree; give one of them, or the same value twice"
fi
[ -n "$VENDOR" ] && [ -z "$MACHINE" ] && MACHINE="$VENDOR"
[ -n "$MACHINE" ] && [ -z "$VENDOR" ] && VENDOR="$MACHINE"
[ -n "$VENDOR" ] || usage_die "no vendor given; pass a name or --machine NAME"
case "$VENDOR" in
  ''|*[!a-zA-Z0-9_-]*) usage_die "invalid vendor name: '$VENDOR'" ;;
esac
VENDOR_DIR="$RENO_DIR/vendors/$VENDOR"
[ -d "$VENDOR_DIR" ] || usage_die "unknown vendor '$VENDOR': no $VENDOR_DIR directory"

case "$HML_MCU_TIMEOUT" in
  ''|*[!0-9]*) usage_die "--timeout must be a positive integer, got '$HML_MCU_TIMEOUT'" ;;
esac
[ "$HML_MCU_TIMEOUT" -ge 5 ] || usage_die "--timeout must be at least 5 seconds"

# PER-VENDOR EMULATION BUDGET.
#
# The stm32f767zi row is the only one that stresses a 64 KiB frame, and a 64 KiB
# HML round trip is genuinely expensive to emulate. Measured on this host with
# the shipped image (host wall-clock, from "Machine started"):
#
#   banner complete          +  0.2 s
#   PHASE crc/beacon/control/frame-codec   +131.4 s
#   PHASE reasm/reasm-cap     +192.6 s
#   DONE result=PASS          +374.1 s
#
# So a 300 s harness kill lands in the MIDDLE of a passing run, which is the
# worst possible outcome: it reports a hang for an image that was about to
# print its verdict. The floor is set above the 374 s measurement with room to
# spare, and the per-line waits inside the .robot are larger still, so a real
# regression still surfaces as a named robot failure instead of a harness kill.
#
# This only ever RAISES a bare default. An explicit --timeout, an HML_MCU_TIMEOUT
# in the environment, or a CTest TIMEOUT property is always left alone, because
# somebody set that on purpose and this script is not entitled to overrule it.
case "$VENDOR" in
  stm32f767zi)
    _min_timeout=1200
    if [ -z "$HML_MCU_TIMEOUT_WAS_SET" ] && [ "$HML_MCU_TIMEOUT" -lt "$_min_timeout" ]; then
      HML_MCU_TIMEOUT="$_min_timeout"
    fi
    ;;
esac
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
  skip "the 'renode' executable was not found (looked in \$HML_RENODE and PATH). Install Renode from https://github.com/renode/renode/releases (a portable build needs mono on Linux or dotnet on newer builds), or set \$HML_RENODE=/path/to/renode. See docs/renode_multi_vendor.md section 3."
fi
[ -x "$HML_RENODE" ] || die "HML_RENODE=$HML_RENODE is not executable"

if [ -z "$HML_RENODE_TEST" ]; then
  HML_RENODE_TEST="$(dirname "$HML_RENODE")/renode-test"
  [ -x "$HML_RENODE_TEST" ] || HML_RENODE_TEST=$(command -v renode-test 2>/dev/null || true)
fi
[ -n "$HML_RENODE_TEST" ] || skip "renode was found at $HML_RENODE but no 'renode-test' wrapper was found next to it or on PATH. renode-test is the Robot Framework entry point this harness drives; point \$HML_RENODE_TEST at it, or run the .robot file by hand. See docs/renode_multi_vendor.md section 3."
[ -x "$HML_RENODE_TEST" ] || die "HML_RENODE_TEST=$HML_RENODE_TEST is not executable"

# renode-test is a python3 program and its first import is `robot`. Without the
# Robot Framework it exits 1 with an ImportError, which would read as a failed
# emulator run. That is not what is happening, so it is a skip -- and it says so.
if ! HML_RENODE_PYTHON_PATH="$HML_RENODE_PYTHON_PATH" PATH="$HML_RENODE_PYTHON_PATH:$PATH" \
     python3 -c 'import robot, psutil, yaml' >/dev/null 2>&1; then
  skip "python3 on PATH (plus \$HML_RENODE_PYTHON_PATH='$HML_RENODE_PYTHON_PATH') cannot 'import robot, psutil, yaml', which is what renode-test needs to run a .robot file. Install them into the interpreter that runs renode-test, or point \$HML_RENODE_PYTHON_PATH at a venv that has them. See docs/renode_multi_vendor.md section 3."
fi

# The vendor.env decides the actual cross compiler used, so for the probe we
# only need to know that build_vendor.sh can be asked to build. It exits 77
# with the precise vendor-specific toolchain message if the compiler is absent.
if [ -z "$ELF_OVERRIDE" ]; then
  if ! command -v bash >/dev/null 2>&1; then
    skip "bash is not on PATH; build_vendor.sh cannot be invoked"
  fi
fi

printf 'run_vendor_smoke.sh: vendor      : %s\n' "$VENDOR"
printf 'run_vendor_smoke.sh: machine     : %s\n' "$MACHINE"
printf 'run_vendor_smoke.sh: renode      : %s\n' "$HML_RENODE"
printf 'run_vendor_smoke.sh: renode-test : %s\n' "$HML_RENODE_TEST"
printf 'run_vendor_smoke.sh: python path : %s\n' "${HML_RENODE_PYTHON_PATH:-<unchanged>}"
printf 'run_vendor_smoke.sh: timeout     : %ss\n' "$HML_MCU_TIMEOUT"

# ---- build the image (or accept the one the caller supplied) ----------------
OUT_DIR="${HML_MCU_OUT//%v/$VENDOR}"
mkdir -p "$OUT_DIR"
ELF="$ELF_OVERRIDE"
if [ -z "$ELF" ]; then
  printf 'run_vendor_smoke.sh: building the %s image (stub RTOS, no lwIP)...\n' "$VENDOR"
  set +e
  # shellcheck disable=SC2086  # FRAME_BYTES_ARG/VALUE are optional word-split args
  timeout "$HML_MCU_BUILD_TIMEOUT" "$SCRIPT_DIR/build_vendor.sh" "$VENDOR" --out "$OUT_DIR" \
    $FRAME_BYTES_ARG ${FRAME_BYTES_VALUE:+"$FRAME_BYTES_VALUE"}
  build_rc=$?
  set -e
  if [ "$build_rc" = "124" ]; then
    die "build_vendor.sh timed out after ${HML_MCU_BUILD_TIMEOUT}s"
  fi
  if [ "$build_rc" = "$HML_SKIP_RC" ]; then
    skip "build_vendor.sh reported the $VENDOR cross toolchain is unavailable (exit 77)"
  fi
  [ "$build_rc" = "0" ] || die "build_vendor.sh failed (exit $build_rc)"
  ELF="$OUT_DIR/$(grep '^HML_VENDOR_ELF=' "$VENDOR_DIR/firmware/vendor.env" | cut -d= -f2)"
fi

[ -f "$ELF" ] || die "the firmware image is missing: $ELF"
# Absolute, because renode-test runs from a scratch results directory and
# "sysbus LoadELF" resolves a relative path against THAT, not against the
# caller. Also: Renode's monitor tokenises on whitespace, so a path with a
# space in it cannot be handed over as a monitor variable. Say so rather than
# produce an unexplained "Parameters did not match the signature".
case "$ELF" in
  *' '*)
    die "the image path contains a space, which Renode's monitor cannot pass as a variable: $ELF"
    ;;
esac
ELF=$(cd "$(dirname "$ELF")" && printf '%s/%s' "$(pwd)" "$(basename "$ELF")")
printf 'run_vendor_smoke.sh: image       : %s (%s bytes)\n' "$ELF" "$(wc -c <"$ELF" | tr -d ' ')"
ROBOT_FILE="$VENDOR_DIR/$VENDOR.robot"
[ -f "$ROBOT_FILE" ] || die "the robot test is missing: $ROBOT_FILE"

# ---- run the robot test -----------------------------------------------------
# renode-test 1.17 has no --console/--hide-results (they died with the old
# runner), and its wrapper ALWAYS passes "-r $(pwd)", so the Robot results,
# the per-test logs/ and the failure snapshots/ all land in the current working
# directory. Run it from a scratch directory of its own so the repository never
# collects emulator droppings, and so a killed run cannot leave a half-written
# results tree behind that the next run would trip over.
LOG="$OUT_DIR/${VENDOR}_smoke.log"
RESULTS_DIR="$OUT_DIR/renode_results"
rm -f "$LOG"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

printf 'run_vendor_smoke.sh: running renode-test (max %ss)...\n' "$HML_MCU_TIMEOUT"
set +e
# shellcheck disable=SC2086  # RESULTS_DIR is quoted below; the env assignment is word-split-free
(
  cd "$RESULTS_DIR" || exit 125
  PATH="$HML_RENODE_PYTHON_PATH:$PATH"
  export PATH
  timeout --signal=TERM --kill-after=15 "$HML_MCU_TIMEOUT" \
    "$HML_RENODE_TEST" \
      --show-log \
      --save-logs=always \
      --variable "MACHINE:$MACHINE" \
      --variable "ELF:$ELF" \
      "$ROBOT_FILE"
) >"$LOG" 2>&1
rc=$?
set -e
# The UART markers live in the Renode log the terminal tester wrote, not in the
# Robot summary, so fold them into $LOG: without this the printed evidence would
# be a PASS and nothing else.
if [ -d "$RESULTS_DIR/logs" ]; then
  {
    printf '\nrun_vendor_smoke.sh: ---- UART output captured by the terminal tester ----\n'
    cat "$RESULTS_DIR"/logs/*.log 2>/dev/null || true
  } >>"$LOG"
fi

case "$rc" in
  0)
    printf '\nrun_vendor_smoke.sh: ---- renode-test output ----\n'
    sed 's/^/  /' "$LOG"
    printf 'run_vendor_smoke.sh: PASS -- the emulated %s printed result=PASS.\n' "$VENDOR"
    printf 'run_vendor_smoke.sh: what that proves: this C code runs on the emulated %s\n' "$MACHINE"
    printf 'run_vendor_smoke.sh: machine and the codec/CRC/reassembly checks passed inside it.\n'
    printf 'run_vendor_smoke.sh: what it does NOT prove: FreeRTOS (the RTOS is a cooperative\n'
    printf 'run_vendor_smoke.sh: stub), lwIP (absent), any datagram on any network, any sensor,\n'
    printf 'run_vendor_smoke.sh: and any real silicon. An emulator is not an MCU.\n'
    if [ "$HML_MCU_KEEP_LOG" = "1" ]; then
      printf 'run_vendor_smoke.sh: log kept at %s\n' "$LOG"
    else
      rm -f "$LOG"
    fi
    exit 0
    ;;
  124)
    printf 'run_vendor_smoke.sh: ---- renode-test output (tail) ----\n' >&2
    tail -n 40 "$LOG" | sed 's/^/  /' >&2
    printf 'run_vendor_smoke.sh: [fail] renode-test exceeded %ss and was killed.\n' "$HML_MCU_TIMEOUT" >&2
    printf 'run_vendor_smoke.sh: [fail] A hang is a FAILURE, never a skip: the image ran but the\n' >&2
    printf 'run_vendor_smoke.sh: [fail] assertions never completed. Full log: %s\n' "$LOG" >&2
    exit 1
    ;;
  *)
    printf 'run_vendor_smoke.sh: ---- renode-test output ----\n' >&2
    sed 's/^/  /' "$LOG" >&2
    printf 'run_vendor_smoke.sh: [fail] renode-test exited %s. The tooling was present, so this\n' "$rc" >&2
    printf 'run_vendor_smoke.sh: [fail] is a real failure and is deliberately NOT converted into\n' >&2
    printf 'run_vendor_smoke.sh: [fail] a skip. Full log: %s\n' "$LOG" >&2
    exit 1
    ;;
esac
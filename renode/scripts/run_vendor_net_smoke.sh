#!/usr/bin/env bash
#
# run_vendor_net_smoke.sh -- build and run the NETWORK acceptance test for the
#                            vendor row whose Renode platform declares a MAC.
#
# ---------------------------------------------------------------------------
# WHY THIS IS NOT run_vendor_smoke.sh WITH A FLAG
# ---------------------------------------------------------------------------
# run_vendor_smoke.sh builds the CODEC image and runs a test whose central
# assertion is that no datagram ever leaves it. This builds the NETWORK image and
# runs a test whose central assertion is that a real HML datagram DOES leave it.
# Sharing a runner would mean one flag selecting between two opposite tests, and
# the failure mode of getting that wrong -- a green codec smoke that is really a
# network run, or the reverse -- is exactly the kind of quiet substitution that
# makes an acceptance suite untrustworthy. So the two are separate commands,
# separate ELFs, separate .robot files, separate output names and separate
# results directories.
#
# Everything else is deliberately parallel to run_vendor_smoke.sh: the same exit
# codes (0 pass, 77 skipped, 2 usage, 1 failed), the same environment variables,
# the same "a skip is never a pass" rule, and the same refusal to convert a real
# failure into a skip.
#
# EXIT CODES
#   0  pass       77  skipped      2  usage error      1  the run failed
set -eu
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RENO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)

HML_SKIP_RC=77
: "${HML_CROSS_PREFIX:=}"
: "${HML_MCU_OUT:=$RENO_DIR/build/%v}"
: "${HML_MCU_FRAME_BYTES:=}"
: "${HML_LWIP_BASE:=$HOME/hml_toolchain_cache/lwip}"
: "${HML_RENODE:=}"
: "${HML_RENODE_TEST:=}"
: "${HML_RENODE_PYTHON_PATH:=}"
: "${HML_MCU_TIMEOUT:=900}"
: "${HML_MCU_BUILD_TIMEOUT:=300}"
: "${HML_MCU_KEEP_LOG:=0}"
# Optional word-split args forwarded to the build script; empty unless
# --frame-bytes was given on this command line.
FRAME_BYTES_ARG=""
FRAME_BYTES_VALUE=""
: "${HML_VENDOR_NET_SKIP:=0}"

usage() {
  cat >&2 <<'EOF'
usage: run_vendor_net_smoke.sh VENDOR [--machine NAME] [--renode PATH]
                                    [--renode-test PATH] [--elf PATH]
                                    [--timeout SEC] [--frame-bytes N] [--keep-log]
                                    [--help]

Builds the network image for VENDOR (via build_vendor_net.sh, which needs a
prebuilt liblwip.a from build_lwip.sh) and runs VENDOR_net.robot under Renode.
EOF
}

die()    { printf 'run_vendor_net_smoke.sh: [fail] %s\n' "$*" >&2; exit 1; }
skip()   { printf 'run_vendor_net_smoke.sh: [skip] %s\n' "$*"; exit "$HML_SKIP_RC"; }

VENDOR=""
MACHINE=""
ELF_OVERRIDE=""
ARG_TIMEOUT=""
ARG_FRAME=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --machine) [ "$#" -ge 2 ] || die "--machine needs a value"; MACHINE="$2"; shift 2 ;;
    --renode) [ "$#" -ge 2 ] || die "--renode needs a value"; HML_RENODE="$2"; shift 2 ;;
    --renode-test) [ "$#" -ge 2 ] || die "--renode-test needs a value"; HML_RENODE_TEST="$2"; shift 2 ;;
    --elf) [ "$#" -ge 2 ] || die "--elf needs a value"; ELF_OVERRIDE="$2"; shift 2 ;;
    --timeout) [ "$#" -ge 2 ] || die "--timeout needs a value"; ARG_TIMEOUT="$2"; shift 2 ;;
    --frame-bytes) [ "$#" -ge 2 ] || die "--frame-bytes needs a value"; ARG_FRAME="$2"; shift 2 ;;
    --keep-log) HML_MCU_KEEP_LOG=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown argument: $1" ;;
    *)
      [ -z "$VENDOR" ] || die "vendor given twice: $VENDOR and $1"
      VENDOR="$1"; shift
      ;;
  esac
done
[ -n "$VENDOR" ] || { usage; exit 2; }
case "$VENDOR" in
  ''|*[!a-zA-Z0-9_-]*) die "invalid vendor name: '$VENDOR'" ;;
esac

VENDOR_DIR="$RENO_DIR/vendors/$VENDOR"
[ -d "$VENDOR_DIR" ] || die "unknown vendor '$VENDOR'"
[ -f "$VENDOR_DIR/firmware/vendor.env" ] || die "no vendor.env for '$VENDOR'"
# shellcheck disable=SC1090
. "$VENDOR_DIR/firmware/vendor.env"
[ -n "$MACHINE" ] || MACHINE="$HML_VENDOR_MACHINE"
[ -n "$ARG_TIMEOUT" ] && HML_MCU_TIMEOUT="$ARG_TIMEOUT"
if [ -n "$ARG_FRAME" ]; then
  HML_MCU_FRAME_BYTES="$ARG_FRAME"
  FRAME_BYTES_ARG="--frame-bytes"
  FRAME_BYTES_VALUE="$ARG_FRAME"
fi

[ "$HML_MCU_TIMEOUT" -ge 5 ] 2>/dev/null || die "--timeout must be at least 5 seconds"
[ "$HML_VENDOR_NET_SKIP" = "1" ] && skip "HML_VENDOR_NET_SKIP=1 is set (forced by the CTest self-check)"

command -v timeout >/dev/null 2>&1 || die "'timeout' is not on PATH; refusing to run an unbounded command"

# ---- toolchain probe ---------------------------------------------------------
[ -n "$HML_RENODE" ] || HML_RENODE="$(command -v renode 2>/dev/null || true)"
if [ -z "$HML_RENODE" ]; then
  for c in "$HOME"/hml_toolchain_cache/renode*portable/renode /opt/renode/renode; do
    [ -x "$c" ] && { HML_RENODE="$c"; break; }
  done
fi
[ -n "$HML_RENODE" ] || skip "no Renode found (set HML_RENODE, or --renode PATH)"
[ -x "$HML_RENODE" ] || die "HML_RENODE=$HML_RENODE is not executable"
[ -n "$HML_RENODE_TEST" ] || HML_RENODE_TEST="$(dirname "$HML_RENODE")/renode-test"
[ -x "$HML_RENODE_TEST" ] || HML_RENODE_TEST=$(command -v renode-test 2>/dev/null || true)
[ -n "$HML_RENODE_TEST" ] || skip "Renode is at $HML_RENODE but no 'renode-test' wrapper is next to it or on PATH. renode-test is the Robot Framework entry point this harness drives."
if ! HML_RENODE_PYTHON_PATH="$HML_RENODE_PYTHON_PATH" PATH="$HML_RENODE_PYTHON_PATH:$PATH" \
     python3 -c 'import robot, psutil, yaml' >/dev/null 2>&1; then
  skip "python3 cannot 'import robot, psutil, yaml', which renode-test needs. Install them, or point \$HML_RENODE_PYTHON_PATH at a venv that has them."
fi
# build_vendor_net.sh, not build_vendor.sh: the netif, the MAC driver and liblwip
# are linked only here.
[ -f "$HML_LWIP_BASE/build/liblwip.a" ] \
  || skip "no liblwip.a at $HML_LWIP_BASE/build/liblwip.a. Run $HML_LWIP_BASE/build_lwip.sh first (see docs/renode_network_example.md); a missing stack is a skip, not a failure."

printf 'run_vendor_net_smoke.sh: vendor      : %s\n' "$VENDOR"
printf 'run_vendor_net_smoke.sh: machine     : %s\n' "$MACHINE"
printf 'run_vendor_net_smoke.sh: renode      : %s\n' "$HML_RENODE"
printf 'run_vendor_net_smoke.sh: renode-test : %s\n' "$HML_RENODE_TEST"
printf 'run_vendor_net_smoke.sh: python path : %s\n' "${HML_RENODE_PYTHON_PATH:-<unchanged>}"
printf 'run_vendor_net_smoke.sh: lwIP        : %s\n' "$HML_LWIP_BASE/build/liblwip.a"
printf 'run_vendor_net_smoke.sh: timeout     : %ss\n' "$HML_MCU_TIMEOUT"

# ---- build the network image -------------------------------------------------
OUT_DIR="${HML_MCU_OUT//%v/$VENDOR}"
mkdir -p "$OUT_DIR"
ELF="$ELF_OVERRIDE"
if [ -z "$ELF" ]; then
  printf 'run_vendor_net_smoke.sh: building the %s NETWORK image (lwIP + MAC driver + udp transport)...\n' "$VENDOR"
  set +e
  # shellcheck disable=SC2086  # FRAME_BYTES_ARG/VALUE are optional word-split args
  timeout "$HML_MCU_BUILD_TIMEOUT" "$SCRIPT_DIR/build_vendor_net.sh" "$VENDOR" --out "$OUT_DIR" \
    $FRAME_BYTES_ARG ${FRAME_BYTES_VALUE:+"$FRAME_BYTES_VALUE"}
  build_rc=$?
  set -e
  [ "$build_rc" = "124" ] && die "build_vendor_net.sh timed out after ${HML_MCU_BUILD_TIMEOUT}s"
  [ "$build_rc" = "$HML_SKIP_RC" ] && skip "build_vendor_net.sh reported the toolchain is unavailable (exit 77)"
  [ "$build_rc" = "0" ] || die "build_vendor_net.sh failed (exit $build_rc)"
  ELF="$OUT_DIR/${VENDOR}_net_demo.elf"
fi
[ -f "$ELF" ] || die "the network image is missing: $ELF"
# Renode's monitor tokenises on whitespace, so a path with a space in it cannot be
# handed over as a monitor variable. Say so rather than produce an unexplained
# "Parameters did not match the signature".
case "$ELF" in
  *' '*) die "the image path contains a space, which Renode's monitor cannot pass as a variable: $ELF" ;;
esac
ELF=$(cd "$(dirname "$ELF")" && printf '%s/%s' "$(pwd)" "$(basename "$ELF")")
printf 'run_vendor_net_smoke.sh: image       : %s (%s bytes)\n' "$ELF" "$(wc -c <"$ELF" | tr -d ' ')"

ROBOT_FILE="$VENDOR_DIR/${VENDOR}_net.robot"
[ -f "$ROBOT_FILE" ] || die "the network robot test is missing: $ROBOT_FILE"

# ---- run it ------------------------------------------------------------------
# The results tree is its own directory, distinct from the codec smoke's, so the two
# can never overwrite each other's evidence.
LOG="$OUT_DIR/${VENDOR}_net_smoke.log"
RESULTS_DIR="$OUT_DIR/renode_results_net"
rm -f "$LOG"
rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

# One mode. Kept as a loop because the results directory and the log fold-in are
# written per mode, and because a second mode would need its own Renode process:
# the suite cannot assert the packet-level evidence and the firmware's own UART
# verdict in one emulation. Renode's terminal tester and its network tester each
# match only while their own wait is pending, the firmware prints its whole verdict
# within a few microseconds of transmitting the datagram, and whichever wait is not
# pending at that moment loses the evidence outright. Pinning the emulation between
# UART waits (defaultPauseEmulation) was tried and is worse: the ARP packet wait
# then never sees a frame because nothing resumes the CPU.
#
# So the run asserts the frame, which is the stronger claim: it reads the bytes the
# emulated PHY carried rather than a log line describing them. The firmware's own
# result=PASS is printed in the same run and visible in the log, but is not a test
# assertion, and the .robot says so where a reader will find it.
#
# A hang is still a failure: a timeout is exit 124 and falls through to the same
# real-failure branch as any other non-zero code.
MODES="wire"

run_mode() {
  local mode="$1"
  local dir="$RESULTS_DIR/$mode"
  mkdir -p "$dir"
  printf 'run_vendor_net_smoke.sh: running renode-test mode=%s (max %ss)...\n' \
    "$mode" "$HML_MCU_TIMEOUT"
  # shellcheck disable=SC2086  # RESULTS_DIR is quoted below; the env assignment is word-split-free
  (
    cd "$dir" || exit 125
    PATH="$HML_RENODE_PYTHON_PATH:$PATH"
    export PATH
    timeout --signal=TERM --kill-after=15 "$HML_MCU_TIMEOUT" \
      "$HML_RENODE_TEST" \
        --show-log \
        --save-logs=always \
        --variable "MACHINE:$MACHINE" \
        --variable "ELF:$ELF" \
        --variable "NET_ASSERT_MODE:$mode" \
        "$ROBOT_FILE"
  ) >>"$LOG" 2>&1
}

overall_rc=0
first_rc=0
for mode in $MODES; do
  set +e
  run_mode "$mode"
  rc=$?
  set -e
  [ "$rc" = "0" ] || overall_rc="$rc"
  [ "$first_rc" = "0" ] && first_rc="$rc"
  printf 'run_vendor_net_smoke.sh: mode=%s exited %s\n' "$mode" "$rc"
done
rc="$overall_rc"

# The UART lines and the packet-level evidence live in the Renode log the testers
# wrote, not in the Robot summary, so fold them into $LOG: without this the printed
# evidence would be a PASS and nothing else.
for mode in $MODES; do
  if [ -d "$RESULTS_DIR/$mode/logs" ]; then
    {
      printf '\nrun_vendor_net_smoke.sh: ---- mode=%s: UART output captured by the terminal tester ----\n' "$mode"
      cat "$RESULTS_DIR/$mode"/logs/*.log 2>/dev/null || true
    } >>"$LOG"
  fi
done

case "$rc" in
  0)
    printf '\nrun_vendor_net_smoke.sh: ---- renode-test output ----\n'
    sed 's/^/  /' "$LOG"
    printf '\nrun_vendor_net_smoke.sh: PASS -- the emulated %s carried a real HML datagram\n' "$MACHINE"
    printf 'run_vendor_net_smoke.sh: The MAC put Ethernet II, IPv4, UDP and the HML header with its magic\n'
    printf 'run_vendor_net_smoke.sh: on the emulated PHY, addressed to 10.0.0.2:5000, and the test read\n'
    printf 'run_vendor_net_smoke.sh: those bytes off the wire rather than trusting a log line. Reaching\n'
    printf 'run_vendor_net_smoke.sh: 10.0.0.2 at all required lwIP to resolve it from an ARP reply the test\n'
    printf 'run_vendor_net_smoke.sh: injected and lwIP processed with its own etharp_input().\n'
    printf 'run_vendor_net_smoke.sh: what is NOT asserted: the firmware result=PASS line it prints in the\n'
    printf 'run_vendor_net_smoke.sh: same run (see the note in the .robot about the two testers), and the\n'
    printf 'run_vendor_net_smoke.sh: UDP checksum, which lwIP computes and nothing here recomputes.\n'
    printf 'run_vendor_net_smoke.sh: what that does NOT prove: silicon (this is a model), DMA\n'
    printf 'run_vendor_net_smoke.sh: coherency (the caches are off, so nothing here would notice),\n'
    printf 'run_vendor_net_smoke.sh: FreeRTOS (it is a stub and this image starts no scheduler),\n'
    printf 'run_vendor_net_smoke.sh: any protocol other than ARP/IPv4/UDP, any real wire, any peer\n'
    printf 'run_vendor_net_smoke.sh: other than this tester, any performance number, and the codec\n'
    printf 'run_vendor_net_smoke.sh: path, which is not in this image at all.\n'
    if [ "$HML_MCU_KEEP_LOG" = "1" ]; then
      printf 'run_vendor_net_smoke.sh: log kept at %s\n' "$LOG"
    else
      rm -f "$LOG"
    fi
    exit 0
    ;;
  124)
    printf 'run_vendor_net_smoke.sh: ---- renode-test output (tail) ----\n' >&2
    tail -n 40 "$LOG" | sed 's/^/  /' >&2
    printf 'run_vendor_net_smoke.sh: [fail] renode-test exceeded %ss and was killed (a mode above\n' "$HML_MCU_TIMEOUT" >&2
    printf 'run_vendor_net_smoke.sh: [fail] exited 124; the failing mode is the last one printed).\n' >&2
    printf 'run_vendor_net_smoke.sh: [fail] A hang is a FAILURE, never a skip. Full log: %s\n' "$LOG" >&2
    exit 1
    ;;
  *)
    printf 'run_vendor_net_smoke.sh: ---- renode-test output ----\n' >&2
    sed 's/^/  /' "$LOG" >&2
    printf 'run_vendor_net_smoke.sh: [fail] renode-test exited %s. The tooling was present, so this\n' "$rc" >&2
    printf 'run_vendor_net_smoke.sh: [fail] is a real failure and is deliberately NOT a skip.\n' >&2
    printf 'run_vendor_net_smoke.sh: [fail] Full log: %s\n' "$LOG" >&2
    exit 1
    ;;
esac

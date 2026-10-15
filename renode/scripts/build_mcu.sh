#!/usr/bin/env bash
#
# build_mcu.sh -- cross-compile the Cortex-M smoke image for the Renode run.
#
# Exit codes (all of them are load-bearing):
#   0   the ELF was built
#   77  SKIPPED: the ARM toolchain is not installed. CTest maps 77 to
#       "Skipped" (SKIP_RETURN_CODE), so a machine without a cross compiler
#       is never a red build. The reason is printed in full.
#   2   usage error: a bad argument. This is a caller bug, never a skip.
#   1   the toolchain is present but the build failed. Loud, on purpose.
#
# WHAT IT BUILDS
#   A single-image Cortex-M4 firmware whose only job is to run the library's
#   codec/CRC/reassembly code and print the result on a UART. The RTOS API it
#   links against is the cooperative STUB in
#   firmware/freertos_demo/stub_freertos/ -- this is NOT a FreeRTOS port and
#   this script does not build one. See docs/renode.md.
#
# WHY THERE IS NO FreeRTOS OR lwIP IN THIS BUILD
#   Neither is available in this repository, and vendoring a fake one would be
#   worse than having none: it would look like a port. The sources below are
#   the library's own portable C plus a deliberately tiny target, and the image
#   sends no datagram, so lwIP has nothing to do. Making this a real
#   FreeRTOS+lwIP validation is described in docs/renode.md section 7; until
#   then this script must not be described as building a FreeRTOS image.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
FW_DIR=$(cd "$SCRIPT_DIR/../firmware/freertos_demo" && pwd)
REPO_ROOT=$(cd "$FW_DIR/../../.." && pwd)

# CTest's "skipped" return code.
HML_SKIP_RC=77

# Defaults, all overridable from the environment for a second bring-up.
: "${HML_CROSS_PREFIX:=arm-none-eabi-}"
: "${HML_CROSS_CC:=${HML_CROSS_PREFIX}gcc}"
: "${HML_CROSS_SIZE:=${HML_CROSS_PREFIX}size}"
: "${HML_MCU_OUT:=${HML_MCU_OUT:-${SCRIPT_DIR}/../build}}"
: "${HML_MCU_FRAME_BYTES:=0}"
: "${HML_MCU_SKIP:=0}"

usage() {
  cat <<'EOF'
usage: build_mcu.sh [--frame-bytes N] [--out DIR] [--cc PATH] [--help]

  --frame-bytes N  cap passed to hml_reasm_open(). 0 (default) selects the
                   library default of 1 MiB, which mallocs TWO 1 MiB buffers:
                   2 MiB of heap, which is why the linker script puts the heap
                   in the SDRAM window. Use a small N on a part with no
                   external SDRAM; the code paths executed are identical.
  --out DIR        directory for hml_mcu_demo.elf (default: renode/build)
  --cc PATH        the cross compiler to use (default: arm-none-eabi-gcc)

Exit: 0 built, 77 skipped (no toolchain), 2 usage error, 1 build failure.
env:   HML_CROSS_PREFIX, HML_CROSS_CC, HML_MCU_OUT, HML_MCU_FRAME_BYTES,
       HML_MCU_SKIP=1 forces the skip path for CI self-testing.
EOF
}

skip() {
  printf 'build_mcu.sh: [skip] %s\n' "$1" >&2
  printf 'build_mcu.sh: [skip] the Cortex-M image cannot be cross-compiled here.\n' >&2
  printf 'build_mcu.sh: [skip] This is NOT a test failure and NOT a pass.\n' >&2
  exit "$HML_SKIP_RC"
}

die() {
  printf 'build_mcu.sh: [fail] %s\n' "$1" >&2
  exit 1
}

usage_die() {
  printf 'build_mcu.sh: %s\n' "$1" >&2
  usage >&2
  exit 2
}

# ---- argument validation (a bad argument is a bug, never a skip) -----------
while [ "$#" -gt 0 ]; do
  case "$1" in
    --frame-bytes)
      [ "$#" -ge 2 ] || usage_die "--frame-bytes needs a value"
      HML_MCU_FRAME_BYTES="$2"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || usage_die "--out needs a value"
      HML_MCU_OUT="$2"
      shift 2
      ;;
    --cc)
      [ "$#" -ge 2 ] || usage_die "--cc needs a value"
      HML_CROSS_CC="$2"
      shift 2
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

case "$HML_MCU_FRAME_BYTES" in
  ''|*[!0-9]*) usage_die "--frame-bytes must be a non-negative integer, got '$HML_MCU_FRAME_BYTES'" ;;
esac
if [ "$HML_MCU_FRAME_BYTES" -gt 1048576 ]; then
  usage_die "--frame-bytes above HML_MAX_FRAME_SIZE (1048576) would be clamped by the library"
fi
[ -n "$HML_MCU_OUT" ] || usage_die "--out must not be empty"

# ---- forced skip, used by the CTest self-check -----------------------------
if [ "$HML_MCU_SKIP" = "1" ]; then
  skip "HML_MCU_SKIP=1 is set (forced by the CTest self-check)"
fi

# ---- toolchain probe -------------------------------------------------------
if ! command -v "$HML_CROSS_CC" >/dev/null 2>&1; then
  skip "the ARM cross compiler '$HML_CROSS_CC' is not on PATH. Install gcc-arm-none-eabi (Debian/Ubuntu: sudo apt-get install gcc-arm-none-eabi; Fedora: sudo dnf install arm-none-eabi-gcc-cs; macOS: brew install --cask gcc-arm-embedded, or arm-none-eabi-gcc from https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads). See docs/renode.md section 1."
fi

printf 'build_mcu.sh: cross compiler : %s\n' "$(command -v "$HML_CROSS_CC")"
printf 'build_mcu.sh: frame cap      : %s (0 = library default 1 MiB, 2 MiB of heap)\n' \
  "$HML_MCU_FRAME_BYTES"
printf 'build_mcu.sh: output dir     : %s\n' "$HML_MCU_OUT"

mkdir -p "$HML_MCU_OUT"
OUT_ELF="$HML_MCU_OUT/hml_mcu_demo.elf"

# ---- sources ---------------------------------------------------------------
# The library's portable C. Nothing socket-shaped is linked: the image has no
# network stack, so it must not contain a socket implementation it never uses.
#   src/protocol : CRC-16, CRC-32, frame header codec, hml_dest_cfg codec
#   src/core     : the reassembler (which also pulls the EOF-latency histogram)
#   src/control  : the beacon and control codecs
# The rest is the demo itself: the check body, the stub RTOS, and the reset
# vector. See firmware/freertos_demo/README.md for the real/stub boundary.
LIB_SRCS="
$REPO_ROOT/src/protocol/hml_crc.c
$REPO_ROOT/src/protocol/hml_protocol.c
$REPO_ROOT/src/core/hml_latency.c
$REPO_ROOT/src/core/hml_reasm.c
$REPO_ROOT/src/control/hml_discovery.c
$REPO_ROOT/src/control/hml_ctrl_codec.c
"
DEMO_SRCS="
$FW_DIR/app/hml_mcu_demo.c
$FW_DIR/stub_freertos/freertos_stub.c
$FW_DIR/platform/startup_stub_rtos.c
"
SRCS="$LIB_SRCS$DEMO_SRCS"

for f in $SRCS; do
  [ -f "$f" ] || die "expected source is missing: $f"
done
[ -f "$FW_DIR/platform/gcc_arm_hml_mcu.ld" ] || die "linker script is missing"
[ -f "$REPO_ROOT/include/hml/hml_types.h" ] || die "the public headers are missing at $REPO_ROOT/include"

CFLAGS="-mcpu=cortex-m4 -mthumb -std=c11 -Os -g
 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections
 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
 -I$REPO_ROOT/include -I$FW_DIR/app -I$FW_DIR/stub_freertos
 -DHML_PORT_FREERTOS -DHML_MCU_FRAME_BYTES=$HML_MCU_FRAME_BYTES"
# HML_PORT_FREERTOS selects the port TAG in hml_port.h only: the real
# ports/freertos_lwip/hml_port_freertos.c is deliberately NOT linked here, and
# hml_clock_us comes from platform/startup_stub_rtos.c instead. Adding -lg here
# would pull newlib's syscall stubs in next to --specs=nosys.specs and produce
# duplicate definitions; nosys is the only stub provider this image needs.
LDFLAGS="-mcpu=cortex-m4 -mthumb -nostartfiles
 -T $FW_DIR/platform/gcc_arm_hml_mcu.ld
 -Wl,--gc-sections -Wl,-Map=$HML_MCU_OUT/hml_mcu_demo.map
 --specs=nano.specs --specs=nosys.specs"

# Build each object in its own bounded step so a hang is attributable and a
# timeout applies to a real command rather than to the whole loop.
OBJ_DIR="$HML_MCU_OUT/obj"
mkdir -p "$OBJ_DIR"
OBJS=""
for f in $SRCS; do
  obj="$OBJ_DIR/$(basename "$f" .c).o"
  # shellcheck disable=SC2086  # CFLAGS is a deliberately word-split list
  if timeout 120 "$HML_CROSS_CC" $CFLAGS -c "$f" -o "$obj"; then
    :
  else
    rc=$?
    [ "$rc" = "124" ] && die "compiling $f timed out after 120s"
    die "compiling $f failed (exit $rc)"
  fi
  OBJS="$OBJS $obj"
done

# shellcheck disable=SC2086  # OBJS is a deliberately word-split list
if timeout 120 "$HML_CROSS_CC" $LDFLAGS $OBJS -o "$OUT_ELF"; then
  :
else
  rc=$?
  [ "$rc" = "124" ] && die "linking timed out after 120s"
  die "linking failed (exit $rc)"
fi

[ -f "$OUT_ELF" ] || die "the linker reported success but produced no $OUT_ELF"

if command -v "$HML_CROSS_SIZE" >/dev/null 2>&1; then
  printf 'build_mcu.sh: size:\n'
  # shellcheck disable=SC2086  # HML_CROSS_SIZE may carry a prefix path only
  "$HML_CROSS_SIZE" "$OUT_ELF" | sed 's/^/  /'
fi

# Report what the image will actually ask the heap for, so the 2 MiB default is
# impossible to miss in a build log.
if [ "$HML_MCU_FRAME_BYTES" = "0" ]; then
  printf 'build_mcu.sh: reasm cap = library default (1048576 B) => 2097152 B of heap\n'
  printf 'build_mcu.sh: the heap is in the SDRAM window by linker script; on a part\n'
  printf 'build_mcu.sh: with no external SDRAM pass --frame-bytes N instead.\n'
else
  printf 'build_mcu.sh: reasm cap = %s B => %s B of heap\n' \
    "$HML_MCU_FRAME_BYTES" "$((HML_MCU_FRAME_BYTES * 2))"
fi

printf 'build_mcu.sh: built %s\n' "$OUT_ELF"
printf 'build_mcu.sh: NOTE stub-RTOS image: not a FreeRTOS port, no lwIP, no datagram.\n'
exit 0

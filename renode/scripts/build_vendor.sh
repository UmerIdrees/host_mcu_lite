#!/usr/bin/env bash
#
# build_vendor.sh -- cross-compile a vendor's Renode smoke image.
#
# Exit codes (all of them are load-bearing):
#   0   the ELF was built
#   77  SKIPPED: the vendor's cross toolchain is not installed. CTest maps 77 to
#       "Skipped" (SKIP_RETURN_CODE), so a machine without a cross compiler is
#       never a red build. The reason is printed in full.
#   2   usage error: a bad argument, or an unrecognised vendor. A caller bug,
#       never a skip.
#   1   the toolchain is present but the build failed. Loud, on purpose.
#
# WHAT IT BUILDS
#   A per-vendor single-image firmware whose only job is to run the library's
#   codec/CRC/reassembly code and print the result on a UART. The RTOS API it
#   links against is the cooperative STUB in
#   firmware/freertos_demo/stub_freertos/ -- this is NOT a FreeRTOS port and
#   this script does not build one. See docs/renode.md and
#   docs/renode_multi_vendor.md.
#
# WHY VENDORED INPUTS, RATHER THAN ONE FLAG SET
#   renode/vendors/<vendor>/firmware/vendor.env holds the per-vendor facts: the
#   ISA flags, the linker script, the startup file, the default --frame-bytes and
#   the cross prefix. The RISC-V row in particular needs a different ISA, a
#   different toolchain and a different stub arrangement than the four ARM rows.
#   That file is machine-checked by renode/scripts/selfcheck_vendors.sh, so the
#   build inputs and the board manifest cannot drift apart.
#
# WHY THE SHARED STUB IS OPTIONAL
#   HML_VENDOR_SHARED_STUB=1 (the default) means the shared freertos_stub.c from
#   firmware/freertos_demo is linked: its tick is the ARMv7-M SysTick COUNTFLAG.
#   HML_VENDOR_SHARED_STUB=0 means it is NOT: on the RISC-V row the whole
#   NOT-FreeRTOS API surface is implemented inside the vendor's own startup_riscv.c
#   with an MMIO machine-timer tick instead. Either way the shared demo body is
#   compiled unchanged.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
RENO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
FW_DIR=$(cd "$RENO_DIR/firmware/freertos_demo" && pwd)
REPO_ROOT=$(cd "$RENO_DIR/.." && pwd)
VENDOR_DIR_ROOT="$RENO_DIR/vendors"

# CTest's "skipped" return code.
HML_SKIP_RC=77

# Defaults, all overridable from the environment for a second bring-up. The
# vendor's vendor.env supplies the real defaults; the empty HML_CROSS_PREFIX
# below means "take the vendor's" rather than silently assuming ARM.
: "${HML_CROSS_PREFIX:=}"
: "${HML_CROSS_CC:=}"
: "${HML_MCU_OUT:=${HML_MCU_OUT:-${RENO_DIR}/build/%v}}"
: "${HML_MCU_FRAME_BYTES:=}"
: "${HML_MCU_SKIP:=0}"

usage() {
  cat <<'EOF'
usage: build_vendor.sh VENDOR [--frame-bytes N] [--out DIR] [--cc PATH]
                       [--prefix P] [--vendor NAME] [--help]

  VENDOR          a vendor directory under renode/vendors/ (stm32f4, nxp_lpc,
                  ti_tm4c, microchip_sam, riscv). Replaced by --vendor NAME
                  when the positional form is inconvenient. Exactly one of the
                  two must be given.
  --frame-bytes N cap passed to hml_reasm_open(). Defaults to the vendor's
                  recorded cap (vendor.env); 0 selects the library default of
                  1 MiB, which mallocs TWO 1 MiB buffers: 2 MiB of heap, which
                  no part in this matrix has. Use a small N on a part with no
                  external SDRAM; the code paths executed are identical.
  --out DIR       directory for the ELF (default: renode/build/<vendor>, or
                  $HML_MCU_OUT with %v replaced by the vendor name)
  --cc PATH       the cross compiler to use (default: <vendor-prefix>gcc)
  --prefix P      the cross toolchain prefix, overriding the vendor's

Exit: 0 built, 77 skipped (no toolchain), 2 usage error, 1 build failure.
env:   HML_CROSS_PREFIX, HML_CROSS_CC, HML_MCU_OUT (use %v for the vendor),
       HML_MCU_FRAME_BYTES, HML_MCU_SKIP=1 forces the skip path.
EOF
}

skip() {
  printf 'build_vendor.sh: [skip] %s\n' "$1" >&2
  printf 'build_vendor.sh: [skip] the %s image cannot be cross-compiled here.\n' "$VENDOR" >&2
  printf 'build_vendor.sh: [skip] This is NOT a test failure and NOT a pass.\n' >&2
  exit "$HML_SKIP_RC"
}

die() {
  printf 'build_vendor.sh: [fail] %s\n' "$1" >&2
  exit 1
}

usage_die() {
  printf 'build_vendor.sh: %s\n' "$1" >&2
  usage >&2
  exit 2
}

# ---- argument validation (a bad argument is a bug, never a skip) -----------
VENDOR=""
POS_VENDOR=""
ARG_FRAME=""
ARG_OUT=""
ARG_CC=""
ARG_PREFIX=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --frame-bytes)
      [ "$#" -ge 2 ] || usage_die "--frame-bytes needs a value"
      ARG_FRAME="$2"
      shift 2
      ;;
    --out)
      [ "$#" -ge 2 ] || usage_die "--out needs a value"
      ARG_OUT="$2"
      shift 2
      ;;
    --cc)
      [ "$#" -ge 2 ] || usage_die "--cc needs a value"
      ARG_CC="$2"
      shift 2
      ;;
    --prefix)
      [ "$#" -ge 2 ] || usage_die "--prefix needs a value"
      ARG_PREFIX="$2"
      shift 2
      ;;
    --vendor)
      [ "$#" -ge 2 ] || usage_die "--vendor needs a value"
      VENDOR="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    -*)
      usage_die "unknown argument: $1"
      ;;
    *)
      [ -z "$POS_VENDOR" ] || usage_die "vendor given twice: $POS_VENDOR and $1"
      POS_VENDOR="$1"
      shift
      ;;
  esac
done

[ -n "$VENDOR" ] && [ -n "$POS_VENDOR" ] && usage_die "vendor given twice: $VENDOR and $POS_VENDOR"
[ -n "$VENDOR" ] || VENDOR="$POS_VENDOR"
[ -n "$VENDOR" ] || usage_die "no vendor given; pass a name or --vendor NAME"
case "$VENDOR" in
  ''|*[!a-zA-Z0-9_-]*) usage_die "invalid vendor name: '$VENDOR'" ;;
esac

VENDOR_DIR="$VENDOR_DIR_ROOT/$VENDOR"
[ -d "$VENDOR_DIR" ] || usage_die "unknown vendor '$VENDOR': no $VENDOR_DIR directory"

# ---- defaults: vendor first, environment second, command line last ---------
VENDOR_ENV="$VENDOR_DIR/firmware/vendor.env"
[ -f "$VENDOR_ENV" ] || die "expected per-vendor build inputs at $VENDOR_ENV"
# shellcheck disable=SC1090
. "$VENDOR_ENV"

: "${HML_CROSS_PREFIX:=$HML_VENDOR_CROSS_PREFIX}"
: "${HML_CROSS_CC:=${HML_CROSS_PREFIX}gcc}"

if [ -n "$ARG_PREFIX" ]; then HML_CROSS_PREFIX="$ARG_PREFIX"; fi
if [ -n "$ARG_CC" ]; then HML_CROSS_CC="$ARG_CC"; fi
if [ -n "$ARG_FRAME" ]; then HML_MCU_FRAME_BYTES="$ARG_FRAME"; fi
if [ -z "$HML_MCU_FRAME_BYTES" ]; then HML_MCU_FRAME_BYTES="$HML_VENDOR_FRAME_BYTES"; fi

case "$HML_MCU_FRAME_BYTES" in
  ''|*[!0-9]*) usage_die "--frame-bytes must be a non-negative integer, got '$HML_MCU_FRAME_BYTES'" ;;
esac
if [ "$HML_MCU_FRAME_BYTES" -gt 1048576 ]; then
  usage_die "--frame-bytes above HML_MAX_FRAME_SIZE (1048576) would be clamped by the library"
fi
[ "$HML_VENDOR_SHARED_STUB" = "0" ] || [ "$HML_VENDOR_SHARED_STUB" = "1" ] \
  || die "vendor.env has HML_VENDOR_SHARED_STUB=$HML_VENDOR_SHARED_STUB (want 0 or 1)"

OUT_DIR="${ARG_OUT:-}"
if [ -z "$OUT_DIR" ]; then
  OUT_DIR="${HML_MCU_OUT//%v/$VENDOR}"
fi
[ -n "$OUT_DIR" ] || usage_die "--out must not be empty"

# ---- forced skip, used by the CTest self-check -----------------------------
if [ "$HML_MCU_SKIP" = "1" ]; then
  skip "HML_MCU_SKIP=1 is set (forced by the CTest self-check)"
fi

# ---- toolchain probe -------------------------------------------------------
if ! command -v "$HML_CROSS_CC" >/dev/null 2>&1; then
  skip "the $HML_VENDOR_ARCH cross compiler '$HML_CROSS_CC' is not on PATH (override with --cc or HML_CROSS_CC). For the ARM rows install gcc-arm-none-eabi; for the RISC-V row install a riscv32-unknown-elf toolchain (Debian/Ubuntu: sudo apt-get install gcc-riscv64-unknown-elf plus the 32-bit multilib, or a riscv32-unknown-elf- target build). See docs/renode_multi_vendor.md section 3."
fi

printf 'build_vendor.sh: vendor        : %s (%s, %s)\n' "$VENDOR" "$HML_VENDOR_ARCH" "$HML_VENDOR_MACHINE"
printf 'build_vendor.sh: cross compiler: %s\n' "$(command -v "$HML_CROSS_CC")"
printf 'build_vendor.sh: frame cap     : %s bytes\n' "$HML_MCU_FRAME_BYTES"
printf 'build_vendor.sh: shared stub   : %s%s\n' \
  "$([ "$HML_VENDOR_SHARED_STUB" = "1" ] && echo linked || echo NOT-linked)" \
  "$([ "$HML_VENDOR_SHARED_STUB" = "1" ] && echo "" || echo " (tick comes from startup_riscv.c)")"
printf 'build_vendor.sh: output dir    : %s\n' "$OUT_DIR"

mkdir -p "$OUT_DIR"
OUT_ELF="$OUT_DIR/$HML_VENDOR_ELF"

# ---- sources ---------------------------------------------------------------
# The library's portable C is identical to build_mcu.sh's: CRC, protocol,
# latency, reasm, discovery, control codec. Nothing socket-shaped is linked.
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
"
if [ "$HML_VENDOR_SHARED_STUB" = "1" ]; then
  DEMO_SRCS="$DEMO_SRCS
$FW_DIR/stub_freertos/freertos_stub.c
"
fi
DEMO_SRCS="$DEMO_SRCS
$VENDOR_DIR/firmware/$HML_VENDOR_STARTUP
"
SRCS="$LIB_SRCS$DEMO_SRCS"

for f in $SRCS; do
  [ -f "$f" ] || die "expected source is missing: $f"
done
[ -f "$VENDOR_DIR/firmware/platform/$HML_VENDOR_LINKER_SCRIPT" ] || die "linker script is missing"
[ -f "$VENDOR_DIR/firmware/platform/board_config.h" ] || die "board_config.h is missing"
[ -f "$REPO_ROOT/include/hml/hml_types.h" ] || die "the public headers are missing at $REPO_ROOT/include"

CFLAGS="$HML_VENDOR_ISA_FLAGS -std=c11 -Os -g
 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections
 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
 -I$REPO_ROOT/include -I$FW_DIR/app -I$FW_DIR/stub_freertos
 -I$VENDOR_DIR/firmware/platform
 -DHML_PORT_FREERTOS -DHML_MCU_FRAME_BYTES=$HML_MCU_FRAME_BYTES"
# HML_PORT_FREERTOS selects the port TAG in hml_port.h only: the real
# ports/freertos_lwip/hml_port_freertos.c is deliberately NOT linked here, and
# hml_clock_us comes from the vendor's startup file instead. The RISC-V row
# must not consume the ARM SysTick implementation, so HML_VENDOR_SHARED_STUB
# decides whether freertos_stub.c is in SRCS above.
LDFLAGS="$HML_VENDOR_ISA_FLAGS $HML_VENDOR_EXTRA_LDFLAGS -nostartfiles
 -T $VENDOR_DIR/firmware/platform/$HML_VENDOR_LINKER_SCRIPT
 -Wl,--gc-sections -Wl,-Map=$OUT_DIR/${HML_VENDOR_ELF%.elf}.map
 $HML_VENDOR_LIB_SPECS"

# Build each object in its own bounded step so a hang is attributable and a
# timeout applies to a real command rather than to the whole loop.
OBJ_DIR="$OUT_DIR/obj"
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

[ -f "$OUT_ELF" ] || die "the linker produced no $OUT_ELF"
printf 'build_vendor.sh: built %s (frame cap %s bytes, stub %s).\n' \
  "$OUT_ELF" "$HML_MCU_FRAME_BYTES" \
  "$([ "$HML_VENDOR_SHARED_STUB" = "1" ] && echo linked || echo "not-linked (startup-owned tick)")"
printf 'build_vendor.sh: a successful build here proves NOTHING about execution.\n'
printf 'build_vendor.sh: run run_vendor_smoke.sh %s to (try to) run it under Renode.\n' "$VENDOR"
exit 0
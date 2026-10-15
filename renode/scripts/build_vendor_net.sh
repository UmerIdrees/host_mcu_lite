#!/usr/bin/env bash
#
# build_vendor_net.sh -- build the NETWORK image for the vendor row whose Renode
#                       platform declares a modelled MAC.
#
# ---------------------------------------------------------------------------
# WHY THIS IS A SEPARATE SCRIPT AND NOT A FLAG ON build_vendor.sh
# ---------------------------------------------------------------------------
# The codec smoke is a PASSING acceptance test for the stm32f767zi row, and its
# robot test asserts that no datagram ever leaves the image. The network image
# does the opposite: it must put a real HML/UDP datagram on the emulated wire.
# Those are two different ELFs, two different demo bodies, two different
# acceptance tests, and two opposite meanings of "pass".
#
# So they are separate commands with separate outputs, and this script does not
# touch build_vendor.sh's source list, its output ELF name, or anything the codec
# path depends on. A change here cannot regress the codec smoke, and that
# property is the entire reason this is a separate file rather than a --network
# flag on the other one.
#
# ---------------------------------------------------------------------------
# WHAT IT LINKS THAT build_vendor.sh DOES NOT
# ---------------------------------------------------------------------------
#   - liblwip.a, the prebuilt lwIP 2.2.1 core (NO_SYS; UDP, ICMP and ARP on; TCP
#     off). It is NOT built here: it needs its own include path and its own
#     -Dsys_now rename, and rebuilding it on every firmware edit would make a
#     firmware change look like a stack change. build_lwip.sh builds it.
#   - firmware/net/*.c: the MAC driver, the netif, and the hml_sock port.
#   - src/core/hml_transport.c, which the codec image never links because it links
#     no transport at all.
#
# The codec-only modules (latency, reasm, discovery, control codec) are NOT
# linked: this image exercises the transport path, and linking modules it never
# calls would only inflate the map and invite someone to believe they are covered.
set -eu
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RENO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(cd "$RENO_DIR/.." && pwd)
VENDOR_DIR_ROOT="${RENO_DIR}/vendors"
SHARED_FW="${RENO_DIR}/firmware/freertos_demo"

# lwIP lives outside the repo on purpose: it is a third-party dependency, not
# project source, so its version and its licence are a separate, checkable thing
# and it never becomes part of this repository's own tree.
LWIP_BASE="${HML_LWIP_BASE:-$HOME/hml_toolchain_cache/lwip}"
LWIP_LIB="${LWIP_BASE}/build/liblwip.a"
LWIP_SRC="${LWIP_BASE}/lwip-2.2.1/src"
LWIP_BUILD_INC="${LWIP_BASE}/build/include"

die() { printf 'build_vendor_net.sh: %s\n' "$*" >&2; exit 1; }
usage() {
  cat >&2 <<'EOF'
usage: build_vendor_net.sh VENDOR [--out DIR] [--cc CC] [--prefix PREFIX] [--frame-bytes N]

Builds the NETWORK demo image for the vendor row whose .resc declares a modelled
MAC: lwIP 2.2.1 + that row's MAC driver + the library's own UDP transport.

build_lwip.sh must have been run first, with the same float ABI as the vendor.
EOF
}

VENDOR=""
ARG_OUT=""
ARG_CC=""
ARG_PREFIX=""
ARG_FRAME=""
POS=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --out) [ "$#" -ge 2 ] || die "--out needs a value"; ARG_OUT="$2"; shift 2 ;;
    --cc) [ "$#" -ge 2 ] || die "--cc needs a value"; ARG_CC="$2"; shift 2 ;;
    --prefix) [ "$#" -ge 2 ] || die "--prefix needs a value"; ARG_PREFIX="$2"; shift 2 ;;
    --frame-bytes) [ "$#" -ge 2 ] || die "--frame-bytes needs a value"; ARG_FRAME="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown argument: $1" ;;
    *)
      [ "$POS" -eq 0 ] || die "vendor given twice: $VENDOR and $1"
      VENDOR="$1"; POS=1; shift
      ;;
  esac
done
[ -n "$VENDOR" ] || { usage; exit 2; }
case "$VENDOR" in
  ''|*[!a-zA-Z0-9_-]*) die "invalid vendor name: '$VENDOR'" ;;
esac

VENDOR_DIR="$VENDOR_DIR_ROOT/$VENDOR"
[ -d "$VENDOR_DIR" ] || die "unknown vendor '$VENDOR': no $VENDOR_DIR directory"

VENDOR_ENV="$VENDOR_DIR/firmware/vendor.env"
[ -f "$VENDOR_ENV" ] || die "expected per-vendor build inputs at $VENDOR_ENV"
# Sourced, not re-derived: these are the values selfcheck_vendors.sh cross-checks
# against renode/boards/index, the .resc, board_config.h and the linker script.
# Re-deriving any of them here would create a second source of truth for the
# memory map, which is precisely the drift the self-check exists to prevent.
# shellcheck disable=SC1090
. "$VENDOR_ENV"

# ---- net capability ---------------------------------------------------------
# The gate is the platform description, not a name in this script: a row that
# declares no MAC has nothing for a netif to attach to, and building a network
# image for it would produce an ELF that cannot pass. Checking the .resc means
# adding a second network row later needs no edit here.
[ -f "$VENDOR_DIR/$HML_VENDOR_MACHINE.resc" ] || die "no $HML_VENDOR_MACHINE.resc for '$VENDOR'"
if ! grep -Eq '^[[:space:]]*[a-z0-9_]+:[[:space:]]*Network\.(Synopsys)?EthernetMAC\b' \
        "$VENDOR_DIR/$HML_VENDOR_MACHINE.resc"; then
  die "vendor '$VENDOR' declares no Network.EthernetMAC in $HML_VENDOR_MACHINE.resc;
    there is no peripheral for a netif to attach to. Refusing to build a network image."
fi
NET_DIR="$VENDOR_DIR/firmware/net"
[ -d "$NET_DIR" ] || die "vendor '$VENDOR' has a MAC but no $NET_DIR driver directory"

# ---- lwIP prerequisites -----------------------------------------------------
[ -f "$LWIP_LIB" ] || die "liblwip.a not found at $LWIP_LIB -- run $LWIP_BASE/build_lwip.sh first"
[ -d "$LWIP_SRC" ] || die "lwIP sources not found at $LWIP_SRC"
# The dfsg rep of lwIP omits src/include/arch/cc.h; build_lwip.sh generates it.
# lwIP includes it from every source file, so a missing one is a compile error
# that looks like a bad include path.
[ -f "$LWIP_BUILD_INC/arch/cc.h" ] \
  || die "no generated arch/cc.h at $LWIP_BUILD_INC/arch/cc.h -- re-run $LWIP_BASE/build_lwip.sh"

# ---- toolchain, and the float ABI it has to agree with ----------------------
: "${HML_CROSS_PREFIX:=$HML_VENDOR_CROSS_PREFIX}"
: "${HML_CROSS_CC:=${HML_CROSS_PREFIX}gcc}"
if [ -n "$ARG_PREFIX" ]; then HML_CROSS_PREFIX="$ARG_PREFIX"; fi
if [ -n "$ARG_CC" ]; then HML_CROSS_CC="$ARG_CC"; fi
command -v "$HML_CROSS_CC" >/dev/null 2>&1 \
  || die "the $HML_VENDOR_ARCH cross compiler '$HML_CROSS_CC' is not on PATH (override with --cc or HML_CROSS_CC)"

READELF="${HML_CROSS_PREFIX}readelf"
command -v "$READELF" >/dev/null 2>&1 || READELF=readelf

# The archive's own object headers record the ABI it was built with. Comparing
# them against the vendor's -mfloat-abi turns a confusing link failure ("uses VFP
# register arguments ... does not") into a clear one, and names the exact rebuild
# that fixes it. This is a real trap in this project: the codec image never links
# lwIP, so a soft-float stack goes unnoticed until the network build is the first
# thing that ever combines the two.
want_abi=soft
case "$HML_VENDOR_ISA_FLAGS" in
  *-mfloat-abi=hard*) want_abi=hard ;;
esac
lib_abi=unknown
# The tag's value is the human-readable "VFP registers", not an enum name, so the
# match is on the words and not on a fixed token: anything reading this wrongly
# reports "unknown" and then refuses to build a perfectly good hard-float stack.
abi_tags=$("$READELF" -A "$LWIP_LIB" 2>/dev/null \
           | sed -n 's/.*Tag_ABI_VFP_args:[[:space:]]*//p' | sort -u | tr '\n' ';')
case "$abi_tags" in
  *VFP\ registers*) lib_abi=hard ;;
  *Base\ VFP*) lib_abi=soft ;;
esac
if [ "$want_abi" = hard ] && [ "$lib_abi" != hard ]; then
  die "liblwip.a is -mfloat-abi=$lib_abi but vendor '$VENDOR' is hard-float.
    Mixing ABIs in one image fails to link with 'uses VFP register arguments'.
    Rebuild the stack to match, then re-run this script:
        sed -i 's/-mfloat-abi=[a-z]*//; s/^CFLAGS=\"/CFLAGS=\"-mfloat-abi=hard -mfpu=fpv5-d16 /' $LWIP_BASE/build_lwip.sh
        $LWIP_BASE/build_lwip.sh"
fi

if [ -n "$ARG_FRAME" ]; then HML_MCU_FRAME_BYTES="$ARG_FRAME"; fi
: "${HML_MCU_FRAME_BYTES:=$HML_VENDOR_FRAME_BYTES}"
case "$HML_MCU_FRAME_BYTES" in
  ''|*[!0-9]*) die "--frame-bytes must be a non-negative integer, got '$HML_MCU_FRAME_BYTES'" ;;
esac
[ "$HML_VENDOR_SHARED_STUB" = "0" ] || [ "$HML_VENDOR_SHARED_STUB" = "1" ] \
  || die "vendor.env has HML_VENDOR_SHARED_STUB=$HML_VENDOR_SHARED_STUB (want 0 or 1)"

# Same default as run_vendor_smoke.sh, so running this script directly puts the
# ELF where the smoke runner expects to find vendor images.
: "${HML_MCU_OUT:=$RENO_DIR/build/%v}"
OUT_DIR="${ARG_OUT:-${HML_MCU_OUT//%v/$VENDOR}}"
# A distinct ELF name from the codec image, in the same directory. If these ever
# collided, the codec smoke would silently start running the network body, whose
# first assertion is the opposite of the codec smoke's.
OUT_ELF="$OUT_DIR/${VENDOR}_net_demo.elf"

# ---- sources ----------------------------------------------------------------
LIB_SRCS="
$REPO_ROOT/src/protocol/hml_crc.c
$REPO_ROOT/src/protocol/hml_protocol.c
$REPO_ROOT/src/core/hml_transport.c
$REPO_ROOT/src/transports/udp/hml_udp.c
"
NET_SRCS="
$NET_DIR/hml_eth_synopsys.c
$NET_DIR/hml_netif.c
$NET_DIR/hml_sock_lwip.c
"
DEMO_SRCS="
$VENDOR_DIR/firmware/app/hml_mcu_net_demo.c
$VENDOR_DIR/firmware/$HML_VENDOR_STARTUP
"
if [ "$HML_VENDOR_SHARED_STUB" = "1" ]; then
  # The stub supplies xTaskGetTickCount, which is where hml_clock_us() -- and so
  # lwIP's timebase -- comes from. The network image never starts its scheduler;
  # it only needs the tick to move.
  DEMO_SRCS="$DEMO_SRCS
$SHARED_FW/stub_freertos/freertos_stub.c
"
fi
SRCS="$LIB_SRCS$NET_SRCS$DEMO_SRCS"

for f in $SRCS; do
  [ -f "$f" ] || die "expected source is missing: $f"
done
[ -f "$VENDOR_DIR/firmware/platform/$HML_VENDOR_LINKER_SCRIPT" ] || die "linker script is missing"
[ -f "$VENDOR_DIR/firmware/platform/board_config.h" ] || die "board_config.h is missing"
[ -f "$REPO_ROOT/include/hml/hml_types.h" ] || die "the public headers are missing at $REPO_ROOT/include"
[ -f "$REPO_ROOT/src/transports/udp/hml_udp_priv.h" ] \
  || die "hml_udp_priv.h is missing; hml_udp_register() has no other declaration"

# -DHML_VENDOR_APP_NETWORK=1 is what makes the shared startup file call
# hml_mcu_net_demo_run() instead of the codec body, so the macro in the source and
# the macro on this command line cannot drift apart without the link failing on a
# missing hml_mcu_net_demo_run.
#
# -Dsys_now=hml_lwip_now_ms renames lwIP's timebase token to the function
# hml_netif.c defines from the board's own tick. Without it the archive's reference
# to sys_now is undefined and the link fails; that is the intended failure, not a
# bug to paper over by editing the library.
CFLAGS="$HML_VENDOR_ISA_FLAGS -std=c11 -Os -g
 -ffreestanding -fno-builtin -fdata-sections -ffunction-sections
 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Werror
 -I$REPO_ROOT/include -I$REPO_ROOT/src/transports/udp
 -I$SHARED_FW/app -I$SHARED_FW/stub_freertos
 -I$VENDOR_DIR/firmware/platform -I$NET_DIR
 -I$LWIP_SRC/include -I$LWIP_BUILD_INC -I$LWIP_BASE
 -DHML_PORT_FREERTOS -DHML_MCU_FRAME_BYTES=$HML_MCU_FRAME_BYTES
 -DHML_VENDOR_APP_NETWORK=1 -Dsys_now=hml_lwip_now_ms"

LDFLAGS="$HML_VENDOR_ISA_FLAGS $HML_VENDOR_EXTRA_LDFLAGS -nostartfiles
 -T $VENDOR_DIR/firmware/platform/$HML_VENDOR_LINKER_SCRIPT
 -Wl,--gc-sections -Wl,-Map=$OUT_DIR/${VENDOR}_net_demo.map
 $HML_VENDOR_LIB_SPECS"

printf 'build_vendor_net.sh: vendor        : %s (%s, net-capable: MAC declared in %s.resc)\n' \
  "$VENDOR" "$HML_VENDOR_ARCH" "$HML_VENDOR_MACHINE"
printf 'build_vendor_net.sh: cross compiler: %s\n' "$(command -v "$HML_CROSS_CC")"
printf 'build_vendor_net.sh: float ABI     : vendor=%s liblwip=%s\n' "$want_abi" "$lib_abi"
printf 'build_vendor_net.sh: lwIP          : %s\n' "$LWIP_LIB"
printf 'build_vendor_net.sh: frame cap     : %s bytes\n' "$HML_MCU_FRAME_BYTES"
printf 'build_vendor_net.sh: shared stub   : %s\n' \
  "$([ "$HML_VENDOR_SHARED_STUB" = "1" ] && echo linked || echo "not-linked (startup-owned tick)")"
printf 'build_vendor_net.sh: output        : %s\n' "$OUT_ELF"

mkdir -p "$OUT_DIR"
# Each object in its own bounded step, so a hang is attributable to one source
# file and the timeout applies to a real command rather than to the whole loop.
OBJ_DIR="$OUT_DIR/obj-net"
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

# liblwip.a is given as a path rather than via -L/-l so that a stale archive
# somewhere else on the -L path cannot be picked up silently, and so the link line
# reads as the external dependency it is.
# shellcheck disable=SC2086  # OBJS is a deliberately word-split list
if timeout 120 "$HML_CROSS_CC" $LDFLAGS $OBJS "$LWIP_LIB" -o "$OUT_ELF"; then
  :
else
  rc=$?
  [ "$rc" = "124" ] && die "linking timed out after 120s"
  die "linking failed (exit $rc)"
fi
[ -f "$OUT_ELF" ] || die "the linker produced no $OUT_ELF"

# ---- report, and the one number that decides whether a run can even start ---
# The linker script already ASSERTs that the heap and stack fit in SRAM, so a
# true overflow would have failed the link above. What is worth reporting is the
# totals, because lwIP's memp pools are large and static: a reader who sees the
# ram figure can tell at a glance whether the remaining headroom is plausible.
# The two limits live in board_config.h as hex, which is where selfcheck_vendors.sh
# reads them from too. Parsed rather than hard-coded here so this report cannot
# disagree with the header the firmware compiles against.
BOARD_CFG="$VENDOR_DIR/firmware/platform/board_config.h"
# The token is everything up to the first space or comment, with the C u-suffix
# stripped: "0x200000u" and "524288" both come back as arithmetic-ready hex.
macro_num() {
  v=$(sed -n "s/^#define[[:space:]]*$1[[:space:]]*\([^[:space:]]*\).*/\1/p" "$BOARD_CFG" | head -1)
  printf '%s' "${v%u}"
}
flash_bytes=$(macro_num HML_VENDOR_FLASH_BYTES)
sram_bytes=$(macro_num HML_VENDOR_SRAM_BYTES)
[ -n "$flash_bytes" ] || die "could not read HML_VENDOR_FLASH_BYTES from $BOARD_CFG"
[ -n "$sram_bytes" ] || die "could not read HML_VENDOR_SRAM_BYTES from $BOARD_CFG"
# The u-suffix is stripped, so the values are plain hex text; $(( )) reads them.
read -r text data bss _ <<<"$(arm-none-eabi-size "$OUT_ELF" | awk 'NR==2 {print $1, $2, $3, $4}')"
flash_used=$((text + data))
ram_used=$((data + bss))
printf 'build_vendor_net.sh: built %s\n' "$OUT_ELF"
printf 'build_vendor_net.sh: flash %s / %s bytes\n' "$flash_used" "$((flash_bytes))"
printf 'build_vendor_net.sh: ram   %s / %s bytes (data+bss; lwIP memp pools and the MAC rings are in bss)\n' \
  "$ram_used" "$((sram_bytes))"
# The linker script already ASSERTs that heap and stack fit in SRAM, so this is a
# belt-and-braces check on the SRAM total. lwIP's memp pools are the reason it is
# worth stating out loud: they are large and static, so the headroom this image
# leaves is a real constraint on anything added later, not a formality.
[ "$flash_used" -le "$((flash_bytes))" ] || die "the image does not fit in this board's flash"
[ "$ram_used" -le "$((sram_bytes))" ] || die "the image does not fit in this board's SRAM"
printf 'build_vendor_net.sh: a successful build here proves NOTHING about execution:\n'
printf 'build_vendor_net.sh: no frame has been sent, and no ARP has been answered.\n'
printf 'build_vendor_net.sh: run renode/vendors/%s/%s_net.robot to (try to) run it.\n' \
  "$VENDOR" "$VENDOR"
exit 0

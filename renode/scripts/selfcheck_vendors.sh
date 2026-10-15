#!/usr/bin/env bash
#
# selfcheck_vendors.sh -- prove the multi-vendor Renode harness is internally
# consistent, on any machine, with no emulator and no cross toolchain.
#
# WHAT IT CHECKS
#   For every row of renode/boards/index it asserts that all of these agree:
#     renode/boards/index                  the manifest itself
#     renode/vendors/<v>/firmware/vendor.env   the build's inputs
#     renode/vendors/<v>/firmware/platform/board_config.h   the compiler's view
#     renode/vendors/<v>/firmware/platform/<linker script>  the linked map
#     renode/vendors/<v>/<v>.resc          the emulated map
#     renode/vendors/<v>/<v>.robot         the assertions that will run
#   A disagreement in ANY of them is a bug: the image would load into a region
#   the emulated machine does not have, or the robot would wait for a line no
#   firmware prints. This file is that bug surfacing early, on a machine where
#   a Renode run is impossible.
#
# WHAT IT CHECKS, GLOBALLY
#   The index is well-formed and its vendor names are unique. The three helper
#   scripts are executable and pass bash -n. The seven UART markers are defined
#   in app/hml_mcu_demo.h AND named in every vendor's robot file.
#
# THE NEGATIVE TEST
#   Every self-check of this kind can be defeated by a checker that happens to
#   agree with its own bug. So this file also builds a scratch copy of the
#   index, corrupts ONE field of ONE vendor's row, and re-runs itself on that
#   copy. The re-run must FAIL (exit 1). If the corrupted run passes, the
#   checker is not comparing anything and is not trustworthy.
#
# OUTPUT
#   By default each check is printed. --quiet keeps the run clean; the JSON
#   summary block is ALWAYS printed at the end, on stdout, and is the machine
#   interface the CTest wrapper and the bring-up docs rely on. Every field in
#   it is derived from counters incremented by the checks that actually ran.
#
# Exit: 0 all checks passed, 1 any consistency failure, 2 usage error.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(cd "$DEFAULT_ROOT/.." && pwd)
FW_DIR="$DEFAULT_ROOT/firmware/freertos_demo"
SELF="$SCRIPT_DIR/selfcheck_vendors.sh"

ROOT="$DEFAULT_ROOT"
VENDOR_ONLY=""
QUIET=0
NEGATIVE=1
fail=0

usage() {
  cat <<'EOF'
usage: selfcheck_vendors.sh [--root DIR] [--vendor NAME] [--no-negative]
                            [--quiet] [--help]

  --root DIR       the renode directory to check (default: this script's
                   parent). The layout underneath must be boards/ and vendors/.
  --vendor NAME    check only the named vendor's row (default: all rows)
  --no-negative    skip the negative test that corrupts a copy of the index
                   and expects the check to fail
  --quiet          suppress the per-check lines; the JSON summary still prints

Exit: 0 all consistent, 1 consistency failure (or the corrupted-copy re-run
      should have failed and did not), 2 usage error.
env:   HML_VENDOR_RENO_DIR (same as --root)
EOF
}

usage_die() {
  printf 'selfcheck_vendors.sh: %s\n' "$1" >&2
  usage >&2
  exit 2
}

# ---- argument validation --------------------------------------------------
while [ "$#" -gt 0 ]; do
  case "$1" in
    --root)
      [ "$#" -ge 2 ] || usage_die "--root needs a value"
      ROOT="$2"
      shift 2
      ;;
    --vendor)
      [ "$#" -ge 2 ] || usage_die "--vendor needs a value"
      VENDOR_ONLY="$2"
      shift 2
      ;;
    --no-negative)
      NEGATIVE=0
      shift
      ;;
    --quiet)
      QUIET=1
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
: "${HML_VENDOR_RENO_DIR:-}"
if [ -z "$ROOT" ] && [ -n "$HML_VENDOR_RENO_DIR" ]; then ROOT="$HML_VENDOR_RENO_DIR"; fi
if [ -n "$VENDOR_ONLY" ]; then
  case "$VENDOR_ONLY" in
    ''|*[!a-zA-Z0-9_-]*) usage_die "invalid vendor name: '$VENDOR_ONLY'" ;;
  esac
fi

INDEX="$ROOT/boards/index"
VEND_ROOT="$ROOT/vendors"

[ -f "$INDEX" ] || usage_die "no boards/index under --root '$ROOT' ($INDEX)"
[ -d "$VEND_ROOT" ] || usage_die "no vendors/ directory under --root '$ROOT'"

# ---- output helpers -------------------------------------------------------
ok() {
  [ "$QUIET" = "1" ] || printf '  [ ok ] %s\n' "$*"
}
bad() {
  fail=1
  [ "$QUIET" = "1" ] || printf '  [FAIL] %s\n' "$*"
}

# Per-vendor counters. Unset until the first use.
declare -A V_CHECKS V_FAILS
V_ORDER=()

vok()  { V_CHECKS[$1]=$((${V_CHECKS[$1]:-0} + 1)); }
vbad() { V_CHECKS[$1]=$((${V_CHECKS[$1]:-0} + 1)); V_FAILS[$1]=$((${V_FAILS[$1]:-0} + 1)); fail=1; }

# ---- small numeric helpers -------------------------------------------------
# norm_num: '0x200000u' -> 2097152, '32768' -> 32768. Bash must be able to
# evaluate the literal; a malformed board_config.h define is a bad value, not
# a shell crash -- hence the guard.
norm_num() {
  local x="${1%u}"
  local out
  case "$x" in
    ''|*[!0-9xXa-fA-F]*) echo "INVALID:$x" ;;
    0[xX]*|0X*) printf '%d' "$((x))" 2>/dev/null || echo "INVALID:$x" ;;
    *) echo "$x" ;;
  esac
}

# size2bytes: '128K' -> 131072, '2M' -> 2097152, '0x100000' -> 1048576.
size2bytes() {
  local s="${1^^}"
  case "$s" in
    *K) echo $(( ${s%K} * 1024 )) ;;
    *M) echo $(( ${s%M} * 1048576 )) ;;
    *) norm_num "$s" ;;
  esac
}

# hex literal as written in the .resc / index (lowercase, 0x-prefixed).
hexof() { printf '0x%x' "$1"; }

size_literal() {
  local b="$1"
  if [ $(( b % 1048576 )) -eq 0 ]; then
    printf '%dM' $(( b / 1048576 ))
  elif [ $(( b % 1024 )) -eq 0 ]; then
    printf '%dK' $(( b / 1024 ))
  else
    printf '%d' "$b"
  fi
}

# ---- global: index is well formed and vendor names are unique ---------------
G_CHECKS=0
G_FAILS=0
gok()  { G_CHECKS=$((G_CHECKS + 1)); ok "$*"; }
gbad() { G_CHECKS=$((G_CHECKS + 1)); G_FAILS=$((G_FAILS + 1)); bad "$*"; }

bad_rows=0
while IFS= read -r line; do
  nf=$(awk -F'\t' '{print NF}' <<<"$line")
  [ "$nf" = "16" ] || bad_rows=$((bad_rows + 1))
done < <(awk -F'\t' '!/^#/ && $1 != "vendor" { print }' "$INDEX")
if [ "$bad_rows" = "0" ]; then
  gok "index rows all have 16 fields"
else
  gbad "index has $bad_rows row(s) without all 16 fields"
fi

dup=0
seen=""
while IFS= read -r v; do
  [ -n "$v" ] || continue
  if printf '%s\n' "$seen" | grep -qx "$v"; then
    dup=$((dup + 1))
  else
    seen="$seen
$v"
  fi
done < <(awk -F'\t' '!/^#/ && $1 != "vendor" { print $1 }' "$INDEX")
if [ "$dup" = "0" ]; then
  gok "index vendor names are unique"
else
  gbad "index repeats a vendor name $dup time(s)"
fi

# ---- global: the helper scripts exist, are executable, pass bash -n ---------
for s in build_vendor.sh run_vendor_smoke.sh selfcheck_vendors.sh; do
  p="$SCRIPT_DIR/$s"
  if [ ! -f "$p" ]; then
    gbad "$s is missing"
    continue
  fi
  if [ ! -x "$p" ]; then
    gbad "$s is not executable (chmod +x renode/scripts/$s)"
  fi
  bn=$(mktemp)
  if bash -n "$p" 2>"$bn"; then
    gok "$(printf '%-24s' "$s") executable, bash -n clean"
  else
    gbad "$s fails bash -n: $(cat "$bn")"
  fi
  rm -f "$bn"
done

# ---- global: all seven markers exist in the firmware header ------------------
markers="HML-MCU-DEMO-READY HML-MCU-DEMO-PHASE HML-MCU-DEMO-TASK HML-MCU-DEMO-DONE HML-MCU-DEMO-FAIL HML-MCU-DEMO-NOTE HML-MCU-DEMO-WARN"
for m in $markers; do
  if grep -q "$m" "$FW_DIR/app/hml_mcu_demo.h"; then
    gok "marker $m is defined in app/hml_mcu_demo.h"
  else
    gbad "marker $m is missing from app/hml_mcu_demo.h; no robot can rely on it"
  fi
done

# ---- per-vendor checks ------------------------------------------------------
while IFS= read -r line; do
  [ -n "$line" ] || continue
  IFS='|' read -r V M A E UA UI FA FB SA SB HA HB FR LK ST PF <<<"$line"
  [ -n "$V" ] || continue
  if [ -n "$VENDOR_ONLY" ] && [ "$V" != "$VENDOR_ONLY" ]; then
    continue
  fi
  V_ORDER+=("$V")

  VD="$VEND_ROOT/$V"
  ENVF="$VD/firmware/vendor.env"
  BCF="$VD/firmware/platform/board_config.h"
  LKF="$VD/firmware/platform/$LK"
  STARTF="$VD/firmware/platform/$ST"
  RESCF="$VD/$V.resc"
  ROBOTF="$VD/$V.robot"

  for pair in \
      "$VD|vendor directory|d" \
      "$ENVF|vendor.env|f" \
      "$BCF|board_config.h|f" \
      "$LKF|linker script|f" \
      "$STARTF|startup file|f" \
      "$RESCF|platform description ($V.resc)|f" \
      "$ROBOTF|robot test ($V.robot)|f"; do
    f=${pair%%|*}
    rest=${pair#*|}
    what=${rest%%|*}
    kind=${rest##*|}
    if { [ "$kind" = "d" ] && [ -d "$f" ]; } || { [ "$kind" = "f" ] && [ -f "$f" ]; }; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: $what present"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: $what MISSING ($f)"
    fi
  done

  # ---- vendor.env agrees with the index --------------------------------------
  # shellcheck disable=SC1090
  . "$ENVF" 2>/dev/null || { vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: vendor.env does not source cleanly"; }
  for pair in \
      "HML_VENDOR_NAME|$V" \
      "HML_VENDOR_MACHINE|$M" \
      "HML_VENDOR_ARCH|$A" \
      "HML_VENDOR_ELF|$E" \
      "HML_VENDOR_FRAME_BYTES|$FR" \
      "HML_VENDOR_LINKER_SCRIPT|$LK" \
      "HML_VENDOR_CROSS_PREFIX|$PF"; do
    name=${pair%%|*}
    want=${pair##*|}
    got=$(grep "^$name=" "$ENVF" | head -n1 | cut -d= -f2-)
    if [ "$got" = "$want" ]; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: $name matches index"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: $name is $got (index says $want)"
    fi
  done
  stb=$(printf '%s' "${ST##*/}")
  got_st=${HML_VENDOR_STARTUP:-}
  if [ "${got_st##*/}" = "$stb" ]; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: HML_VENDOR_STARTUP names the index's startup file"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: HML_VENDOR_STARTUP is $got_st (index startup $stb)"
  fi

  # ---- the .robot's identity assertion matches what the firmware prints ------
  # startup_<vendor>.c prints  HML_MCU_DEMO-NOTE vendor=$NAME arch=$ARCH  and the
  # .robot waits for exactly that line. If the two drift apart the FIRST assertion
  # can never match, the image is in fact printing correct output, and the smoke
  # test fails on a timeout that reads like a hung firmware. That is not a
  # hypothetical: microchip_sam's robot asked for arch=cortex-m4 while
  # board_config.h said cortex-m7, and the board was reported RED for that alone.
  # Checking that ARCH is a non-empty string is not enough; it has to be the same
  # string the .robot waits for. A regex tail like cortex-m4f is fine when the
  # robot matches with a trailing .*, so this compares only the literal prefix.
  robot_arch=$(grep -ho "vendor=$V arch=[A-Za-z0-9_.-]*" "$ROBOTF" 2>/dev/null | head -n1 | sed 's/.*arch=//' | sed 's/\.\{1,\}$//')
  bc_arch=$(grep '^#define HML_VENDOR_ARCH ' "$BCF" | head -n1 | awk '{print $3}' | tr -d '"')
  # The .robot spells its arch as a regex prefix, so cortex-m4 in a robot is
  # correct for firmware that prints cortex-m4f. Require one to be a prefix of
  # the other; anything else is a real drift that would hang the smoke test.
  arch_ok=0
  if [ -n "$robot_arch" ] && [ -n "$bc_arch" ]; then
    case "$bc_arch" in
      "$robot_arch"*) arch_ok=1 ;;
    esac
    if [ "$arch_ok" -eq 0 ]; then
      case "$robot_arch" in
        "$bc_arch"*) arch_ok=1 ;;
      esac
    fi
  fi
  if [ -z "$robot_arch" ]; then
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: no 'vendor=$V arch=' assertion found in $V.robot"
  elif [ "$arch_ok" -eq 1 ]; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: .robot arch assertion ($robot_arch) matches board_config.h ($bc_arch)"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: .robot waits for arch=$robot_arch but firmware prints arch=$bc_arch"
  fi

  # ---- board_config.h agrees with the index ----------------------------------
  num() { # num <define-name> -- echo the numeric value, or INVALID
    local raw
    raw=$(grep "^#define $1 " "$BCF" | head -n1 | awk '{print $3}')
    norm_num "$raw"
  }
  # Special-case the interrupt line: the index spells "no interrupt controller
  # wired" as "-" and board_config.h must spell the same fact as 0.
  want_irq="$UI"
  [ "$want_irq" = "-" ] && want_irq="0"
  for pair in \
      "HML_VENDOR_NAME|$V|s" \
      "HML_VENDOR_ARCH|$A|s" \
      "HML_VENDOR_ELF_NAME|$E|s" \
      "HML_VENDOR_FLASH_ADDR|$FA|n" \
      "HML_VENDOR_FLASH_BYTES|$FB|n" \
      "HML_VENDOR_SRAM_ADDR|$SA|n" \
      "HML_VENDOR_SRAM_BYTES|$SB|n" \
      "HML_VENDOR_UART_ADDR|$UA|n" \
      "HML_VENDOR_UART_IRQ|$want_irq|n" \
      "HML_VENDOR_FRAME_BYTES|$FR|n" \
      "HML_VENDOR_HEAP_BYTES|$HB|n" \
      "HML_VENDOR_HEAP_ADDR|$HA|n"; do
    name=${pair%%|*}
    rest=${pair#*|}
    want=${rest%%|*}
    kind=${rest##*|}
    if [ "$kind" = "s" ]; then
      got=$(grep "^#define $name " "$BCF" | head -n1 | awk '{print $3}' | tr -d '"')
    else
      got=$(num "$name")
    fi
    # normalize the board's writing style against the index's
    if [ "$kind" = "n" ]; then
      want=$(norm_num "$want")
      got=${got%u}
      [ "$got" != "INVALID:${got#INVALID:}" ] 2>/dev/null || true
      case "$got" in
        INVALID:*) got="$got" ;;
        *) got=$(norm_num "$got") ;;
      esac
    fi
    if [ "$got" = "$want" ]; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: board_config.h $name matches index"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: board_config.h $name is $got (index says $want)"
    fi
  done
  stk=$(num "HML_VENDOR_STACK_BYTES")
  case "$stk" in
    INVALID:*) vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: board_config.h HML_VENDOR_STACK_BYTES missing/invalid" ;;
    0) vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: board_config.h HML_VENDOR_STACK_BYTES is 0" ;;
    *) vok "$V"; [ "$QUIET" = "1" ] || ok "$V: board_config.h HML_VENDOR_STACK_BYTES is non-zero" ;;
  esac

  # ---- the linker script agrees with the index -------------------------------
  if grep -q "^ENTRY(Reset_Handler)" "$LKF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: linker defines ENTRY(Reset_Handler)"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: linker script lost its ENTRY(Reset_Handler)"
  fi
  for pair in \
      "FLASH|$FA|$FB" \
      "SRAM|$SA|$SB"; do
    region=${pair%%|*}
    rest=${pair#*|}
    want_addr=${rest%%|*}
    want_bytes=${rest##*|}
    orig=$(awk -v r="$region" -F'ORIGIN\s*=' '$0 ~ "MEMORY" { inmem=1 } inmem && index($0, r) { for(i=1;i<=NF;i++) if ($i ~ /^ORIGIN/) { print $i; exit } }' "$LKF")
    # simpler: extract ORIGIN/LENGTH for the region line
    line=$(grep -E "^[[:space:]]*$region[[:space:]]+\(rx" "$LKF" || grep -E "^[[:space:]]*$region[[:space:]]+\(rwx" "$LKF" || true)
    if [ -z "$line" ]; then
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: linker script has no $region region"
      continue
    fi
    orig=$(printf '%s' "$line" | grep -o 'ORIGIN = [0-9a-fA-FxX]*' | awk '{print $3}')
    got_orig=$(norm_num "$orig")
    if [ "$got_orig" = "$(norm_num "$want_addr")" ]; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: linker $region ORIGIN matches flash/sram address"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: linker $region ORIGIN is $orig (index says $want_addr)"
    fi
    len=$(printf '%s' "$line" | grep -o 'LENGTH = [0-9a-zA-Z]*' | awk '{print $3}')
    got_len=$(size2bytes "$len")
    if [ "$got_len" = "$(norm_num "$want_bytes")" ]; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: linker $region LENGTH matches index"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: linker $region LENGTH is $len ($got_len bytes, index says $want_bytes)"
    fi
  done
  heap_lit=$(size_literal "$HB")
  if grep -q "@__heap_start__ + $heap_lit" "$LKF" || grep -q "= __heap_start__ + $heap_lit" "$LKF" || grep -q "+ $heap_lit\b" "$LKF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: linker heap literal ($heap_lit) matches index"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: linker script heap literal ($heap_lit) not found for heap_bytes=$HB"
  fi

  # ---- the .resc agrees with the index ---------------------------------------
  if grep -q "^cpu VectorTableOffset $FA$" "$RESCF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: .resc VectorTableOffset is the flash ORIGIN"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: .resc VectorTableOffset is not 'cpu VectorTableOffset $FA'"
  fi
  if grep -q "cpuType: \"$A\"" "$RESCF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: .resc cpuType is $A"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: .resc cpuType is not $A"
  fi
  if grep -q "$UA" "$RESCF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: .resc maps the UART base"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: .resc has no UART base $UA"
  fi
  if [ "$UI" = "-" ]; then
    if ! grep -qE "nvic:|plic:|-> nvic@" "$RESCF"; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: .resc wires no interrupt controller (index irq '-')"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: .resc wires an interrupt controller but the index says uart_irq '-'"
    fi
  else
    if grep -q -- "-> nvic@$UI" "$RESCF"; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: .resc wires the UART to nvic@$UI"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: .resc has no '-> nvic@$UI' for the UART"
    fi
  fi

  # ---- the robot test agrees with the rest of the board ----------------------
  if grep -qF "\${MACHINE}.resc" "$ROBOTF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: robot loads \${MACHINE}.resc"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: robot does not Execute Script \${MACHINE}.resc"
  fi
  if grep -q "HML-MCU-DEMO-NOTE vendor=$V " "$ROBOTF" || grep -q "HML-MCU-DEMO-NOTE vendor=$V" "$ROBOTF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: robot asserts the vendor identity line"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: robot does not assert vendor=$V"
  fi
  if grep -q "image=$E" "$ROBOTF"; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: robot asserts image=$E"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: robot does not assert image=$E"
  fi
  for m in $markers; do
    if grep -q "$m" "$ROBOTF"; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: robot names marker $m"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: robot never names marker $m"
    fi
  done

  # ---- the arithmetic the library depends on ---------------------------------
  hb=$(num "HML_VENDOR_HEAP_BYTES")
  fr=$(num "HML_VENDOR_FRAME_BYTES")
  cb=$(num "HML_VENDOR_REASM_CTX_BYTES")
  ha=$(num "HML_VENDOR_HEAP_ADDR")
  sa=$(num "HML_VENDOR_SRAM_ADDR")
  sb=$(num "HML_VENDOR_SRAM_BYTES")
  if [ "$cb" != "INVALID:${cb#INVALID:}" ] && [ "$cb" -gt 0 ] 2>/dev/null; then
    vok "$V"; [ "$QUIET" = "1" ] || ok "$V: declares a non-zero reassembler-context overhead ($cb bytes) on top of the two buffers"
  else
    vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: HML_VENDOR_REASM_CTX_BYTES is missing or zero; hml_reasm_open() needs the two buffers PLUS its context"
  fi
  if [ "$hb" != "INVALID:${hb#INVALID:}" ] && [ "$fr" != "INVALID:${fr#INVALID:}" ] && [ "$cb" != "INVALID:${cb#INVALID:}" ] 2>/dev/null; then
    if [ "$hb" = "$(( 2 * fr + cb ))" ]; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: heap_bytes ($hb) == 2 * frame_bytes ($fr) + reassembler context ($cb) as hml_reasm_open requires"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: heap_bytes ($hb) is not 2 * frame_bytes ($fr) + reassembler context ($cb) = $(( 2 * fr + cb ))"
    fi
  fi
  if [ "$ha" != "INVALID:${ha#INVALID:}" ] && [ "$sa" != "INVALID:${sa#INVALID:}" ] && [ "$sb" != "INVALID:${sb#INVALID:}" ] 2>/dev/null; then
    if [ "$ha" -ge "$sa" ] && [ $(( ha + hb )) -le $(( sa + sb )) ]; then
      vok "$V"; [ "$QUIET" = "1" ] || ok "$V: the heap lies inside the SRAM window"
    else
      vbad "$V"; [ "$QUIET" = "1" ] || bad "$V: the heap (0x$( printf '%x' "$ha" ) .. 0x$( printf '%x' $(( ha + hb )) )) is outside SRAM (0x$( printf '%x' "$sa" ) .. 0x$( printf '%x' $(( sa + sb )) ))"
    fi
  fi
done < <(awk -F'\t' -v q="$VENDOR_ONLY" '
  !/^#/ && $1 != "vendor" && (q == "" || $1 == q) {
    sep="";
    for (i = 1; i <= 16; i++) { printf "%s%s", sep, $i; sep="|" }
    printf "\n"
  }' "$INDEX")

if [ -n "$VENDOR_ONLY" ] && [ "${#V_ORDER[@]}" = "0" ]; then
  usage_die "--vendor '$VENDOR_ONLY' is not listed in the boards index"
fi

# ---- the negative test -----------------------------------------------------
NEG_OK=0
NEG_RUN="$NEGATIVE"
if [ "$NEGATIVE" = "1" ]; then
  T=$(grep -v '^#' "$INDEX" | awk -F'\t' '$1!="vendor" && $1!=""{print $1; exit}')
  if [ -z "$T" ]; then
    gbad "negative test: no vendor row found to corrupt"
  else
    TMP=$(mktemp -d)
    mkdir -p "$TMP/boards" "$TMP/vendors"
    awk -F'\t' -v tgt="$T" -v OFS='\t' '
      !/^#/ && $1 == tgt { $5 = "0xdeadbeef" }
      { print $0 }' "$INDEX" >"$TMP/boards/index"
    cp -r "$VEND_ROOT/$T" "$TMP/vendors/"
    if "$SELF" --root "$TMP" --vendor "$T" --no-negative --quiet >/dev/null 2>&1; then
      gbad "negative test FAILED TO FAIL: corrupting $T's uart_addr in the index should have been caught"
      NEG_OK=0
    else
      gok "negative test: corrupted $T uart_addr was caught (exit != 0 as required)"
      NEG_OK=1
    fi
    rm -rf "$TMP"
  fi
fi

# ---- JSON summary (always printed, machine-readable) -----------------------
{
  printf '{\n'
  printf '  "tool": "renode/scripts/selfcheck_vendors.sh",\n'
  printf '  "root": "%s",\n' "$ROOT"
  printf '  "per_vendor": {\n'
  first=1
  for v in "${V_ORDER[@]}"; do
    if [ "$first" = "1" ]; then first=0; else printf ',\n'; fi
    ch=${V_CHECKS[$v]:-0}
    fl=${V_FAILS[$v]:-0}
    printf '    "%s": {"ok": %s, "checks": %d, "failures": %d}' "$v" \
      "$( [ "$fl" = "0" ] && echo true || echo false )" "$ch" "$fl"
  done
  printf '\n  },\n'
  printf '  "global": {"checks": %d, "failures": %d},\n' "$G_CHECKS" "$G_FAILS"
  printf '  "negative_test": {"run": %d, "ok": %d},\n' "$NEG_RUN" "$NEG_OK"
  vf=$(IFS=$'\n'; printf '%s\n' "${V_FAILS[*]}" 2>/dev/null | awk '{s+=$1} END{print s+0}')
  vf=${vf:-0}
  printf '  "summary": {"vendors_checked": %d, "vendor_failures": %d, "global_failures": %d%s}\n' \
    "${#V_ORDER[@]}" "$vf" "$G_FAILS" "$( [ "$fail" = "0" ] && echo ', "verdict": "ok"' || echo ', "verdict": "fail"' )"
  printf '}\n'
}

if [ "$fail" = "0" ]; then
  printf 'selfcheck_vendors.sh: all checks passed (exit 0).\n' >&2
  exit 0
fi
printf 'selfcheck_vendors.sh: FAILED (exit 1): report the failures above before trusting any vendor run.\n' >&2
exit 1
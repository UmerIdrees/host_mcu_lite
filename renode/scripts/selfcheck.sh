#!/usr/bin/env bash
#
# selfcheck.sh -- prove the Renode harness fails closed.
#
# This is the CTest test renode_selfcheck. It always runs, on any machine: no
# Renode, no ARM toolchain, no network, no root. It PASSES only when
#
#   * both scripts are executable and syntactically valid bash;
#   * the .resc, the .robot and the firmware sources are present and non-empty;
#   * the UART marker strings in the firmware header and in the robot test have
#     not drifted apart (a rename that misses the .robot would otherwise turn
#     into a mysterious timeout under Renode);
#   * build_mcu.sh and run_smoke.sh exit 77 -- skipped, not failed -- when
#     asked to skip, and say exactly what is missing;
#   * the forced-skip path has no side effects at all: no build directory, no
#     firmware image, no Renode invocation.
#
# Any one of those failing is a bug in the harness, and a harness that cannot
# fail closed must not be trusted to report a pass. Hence: exit 1.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
RENO_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
REPO_ROOT=$(cd "$RENO_DIR/.." && pwd)
FW_DIR="$RENO_DIR/firmware/freertos_demo"
EXPECTED=77
fail=0

printf 'Renode MCU harness: skip-path and integrity self-check (expected exit %s from both scripts)\n' \
  "$EXPECTED"

ok()   { printf '  [ ok ] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*"; fail=1; }

# state_of <dir> -- a stable fingerprint of a directory tree, used to prove the
# forced-skip path created and touched nothing.
state_of() {
  if [ ! -e "$1" ]; then
    printf 'absent'
    return
  fi
  find "$1" -printf '%y %s %T@ %p\n' 2>/dev/null | LC_ALL=C sort | md5sum | cut -d' ' -f1
}

# ---- 1. scripts are executable and valid bash -------------------------------
for s in build_mcu.sh run_smoke.sh selfcheck.sh; do
  p="$SCRIPT_DIR/$s"
  if [ ! -f "$p" ]; then
    bad "$s is missing"
    continue
  fi
  if [ ! -x "$p" ]; then
    bad "$s is not executable (chmod +x renode/scripts/$s)"
    continue
  fi
  bn=$(mktemp)
  if bash -n "$p" 2>"$bn"; then
    ok "$(printf '%-16s' "$s") executable, bash -n clean"
  else
    bad "$s fails bash -n: $(cat "$bn")"
  fi
  rm -f "$bn"
done

# ---- 2. shellcheck, when it happens to be installed ------------------------
if command -v shellcheck >/dev/null 2>&1; then
  # Errors fail the check. Warnings and style notes are printed, not fatal,
  # because the repository does not enforce a shellcheck severity level.
  if out=$(shellcheck -S error "$SCRIPT_DIR/build_mcu.sh" "$SCRIPT_DIR/run_smoke.sh" 2>&1); then
    ok "shellcheck -S error clean on both scripts"
  else
    bad "shellcheck reported errors:"
    printf '%s\n' "$out" | sed 's/^/         /'
  fi
  if shellcheck "$SCRIPT_DIR/build_mcu.sh" "$SCRIPT_DIR/run_smoke.sh" >/dev/null 2>&1; then
    :
  else
    printf '         note: shellcheck has non-error findings; run `shellcheck %s/scripts/*.sh`\n' \
      "$RENO_DIR"
  fi
else
  printf '  [   ] shellcheck not installed; skipped (not required)\n'
fi

# ---- 3. the files the harness needs are present ------------------------------
for f in "$RENO_DIR/holoscan_mcu_lite.resc" \
         "$RENO_DIR/holoscan_mcu_lite.robot" \
         "$FW_DIR/README.md" \
         "$FW_DIR/app/hml_mcu_demo.c" \
         "$FW_DIR/app/hml_mcu_demo.h" \
         "$FW_DIR/platform/startup_stub_rtos.c" \
         "$FW_DIR/platform/main_host_selftest.c" \
         "$FW_DIR/platform/gcc_arm_hml_mcu.ld" \
         "$FW_DIR/stub_freertos/FreeRTOS.h" \
         "$FW_DIR/stub_freertos/freertos_stub.c" \
         "$REPO_ROOT/docs/renode.md"; do
  if [ -s "$f" ]; then
    ok "$(printf '%-52s' "${f#$REPO_ROOT/}") present"
  else
    bad "${f#$REPO_ROOT/} is missing or empty"
  fi
done

# ---- 4. the marker strings match across the firmware and the robot test -----
# Every marker the robot file names must be defined in hml_mcu_demo.h, in both
# directions: a rename in the firmware must break the robot test loudly instead
# of leaving it waiting for a line nobody prints.
markers="HML-MCU-DEMO-READY HML-MCU-DEMO-PHASE HML-MCU-DEMO-TASK HML-MCU-DEMO-DONE HML-MCU-DEMO-FAIL HML-MCU-DEMO-NOTE HML-MCU-DEMO-WARN"
for m in $markers; do
  if grep -q "$m" "$FW_DIR/app/hml_mcu_demo.h" && grep -q "$m" "$RENO_DIR/holoscan_mcu_lite.robot"; then
    ok "marker $m is defined in the firmware and named in the robot test"
  elif ! grep -q "$m" "$FW_DIR/app/hml_mcu_demo.h"; then
    bad "marker $m is named in the robot test but not defined in app/hml_mcu_demo.h"
  else
    bad "marker $m is defined in app/hml_mcu_demo.h but not named in the robot test"
  fi
done

# ---- 5. the stub is labelled as a stub wherever it is read ------------------
if grep -qi "this is not FreeRTOS" "$FW_DIR/stub_freertos/FreeRTOS.h"; then
  ok "stub_freertos/FreeRTOS.h states in the file itself that it is not FreeRTOS"
else
  bad "stub_freertos/FreeRTOS.h does not say 'THIS IS NOT FreeRTOS' any more; put it back"
fi

# ---- 6. both scripts honour the forced skip and exit 77 ---------------------
# Snapshot the build directory first: the forced skip must not create, touch or
# delete anything, even on a box where a real renode_smoke run has already put
# an ELF there.
state_before=$(state_of "$RENO_DIR/build")

for s in build_mcu.sh run_smoke.sh; do
  out=$(HML_MCU_SKIP=1 HML_MCU_SMOKE_SKIP=1 "$SCRIPT_DIR/$s" 2>&1)
  rc=$?
  if [ "$rc" = "$EXPECTED" ]; then
    ok "$(printf '%-16s' "$s") exits $rc (skipped, not failed)"
  else
    bad "$s exits $rc (want $EXPECTED) when forced to skip"
  fi
  if printf '%s' "$out" | grep -qi 'not a pass'; then
    ok "$(printf '%-16s' "$s") says explicitly that the skip is not a pass"
  else
    bad "$s does not say that its skip is not a pass; it said: $(printf '%s' "$out" | tr '\n' '|')"
  fi
done

# ---- 7. a bad argument is a usage error, never a silent skip ---------------
out=$("$SCRIPT_DIR/build_mcu.sh" --frame-bytes not-a-number 2>&1)
rc=$?
if [ "$rc" = "2" ]; then
  ok "build_mcu.sh rejects a bad --frame-bytes with exit 2 (a bug, not a skip)"
else
  bad "build_mcu.sh exits $rc on a bad --frame-bytes; it must be 2 (usage error)"
fi
out=$("$SCRIPT_DIR/run_smoke.sh" --nonsense 2>&1)
rc=$?
if [ "$rc" = "2" ]; then
  ok "run_smoke.sh rejects an unknown argument with exit 2"
else
  bad "run_smoke.sh exits $rc on an unknown argument; it must be 2 (usage error)"
fi

# ---- 8. the forced-skip path has no side effects ---------------------------
state_after=$(state_of "$RENO_DIR/build")
if [ "$state_before" = "$state_after" ]; then
  ok "the forced-skip path had no side effects ($state_after)"
else
  bad "the forced-skip path changed $RENO_DIR/build: before [$state_before] after [$state_after]"
fi

# ---- 9. the memory map is the same on both sides of the emulation boundary --
# The linker script and the Renode platform description must agree, or the image
# loads into a region the emulated machine does not have and the failure looks
# like a wild jump rather than a mismatched map.
LD_SCRIPT="$FW_DIR/platform/gcc_arm_hml_mcu.ld"
RESC="$RENO_DIR/holoscan_mcu_lite.resc"
STARTUP="$FW_DIR/platform/startup_stub_rtos.c"
for region in 0x08000000:flash 0x20000000:sram 0x60000000:sdram_heap; do
  addr=${region%%:*}
  what=${region##*:}
  if grep -q "$addr" "$LD_SCRIPT" && grep -q "$addr" "$RESC"; then
    ok "$what $addr is in both the linker script and the platform description"
  else
    bad "$what $addr is not in both $LD_SCRIPT and $RESC; the emulated map and the linked map must match"
  fi
done
# The USART base the firmware pokes and the one the emulator models.
if grep -q 0x40004400 "$STARTUP" && grep -q 0x40004400 "$RESC"; then
  ok "USART2 0x40004400 is the address both the firmware writes and the emulator models"
else
  bad "USART2 0x40004400 is not in both startup_stub_rtos.c and the platform description"
fi
if grep -q "^cpu VectorTableOffset 0x08000000$" "$RESC"; then
  ok "the vector table offset is the literal flash load address"
else
  bad "the platform description does not set 'cpu VectorTableOffset 0x08000000' literally"
fi

# ---- verdict ----------------------------------------------------------------
if [ "$fail" = "0" ]; then
  ok "harness is intact and fails closed; with Renode installed, renode_smoke runs it for real"
  exit 0
fi
printf '  [FAIL] the Renode harness is not trustworthy: fix the above before trusting any run\n'
exit 1

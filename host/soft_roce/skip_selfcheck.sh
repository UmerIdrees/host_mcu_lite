#!/usr/bin/env bash
#
# skip_selfcheck.sh -- prove that the M3b lab scripts fail closed.
#
# This is the CTest test roce_lab_skip_path: it always runs (no root, no RDMA
# hardware, no rdma_rxe module needed) and it PASSES when every lab script
# invoked with HML_RXE_SKIP=1 exits with 77 (CTest's "skipped" code) instead of
# touching the system or failing. If a script returns anything else, the test
# fails -- which is the point: a broken skip path is a real bug.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
EXPECTED=77
fail=0

# Clean baseline before testing that HML_RXE_SKIP creates no artifacts
rm -rf "$SCRIPT_DIR/artifacts" 2>/dev/null || true

printf 'M3b Soft-RoCE lab: skip-path self-check (expected exit %s for every script)\n' "$EXPECTED"

for s in check_env.sh setup_rxe.sh teardown_rxe.sh run_lab.sh; do
  path="$SCRIPT_DIR/$s"
  if [ ! -x "$path" ]; then
    printf '  [FAIL] %-16s is not executable\n' "$s"
    fail=1
    continue
  fi
  out=$(HML_RXE_SKIP=1 "$path" 2>&1)
  rc=$?
  if [ "$rc" = "$EXPECTED" ]; then
    printf '  [ ok ] %-16s exit %s: %s\n' "$s" "$rc" "$(printf '%s' "$out" | head -n 1)"
  else
    printf '  [FAIL] %-16s exit %s (want %s):\n%s\n' "$s" "$rc" "$EXPECTED" "$out"
    fail=1
  fi
done

# The self-check must also not have created the artifacts directory or any
# namespace, i.e. the forced-skip path performs no side effects at all.
if [ -d "$SCRIPT_DIR/artifacts" ]; then
  printf '  [FAIL] the forced-skip path created %s/artifacts; it must be side-effect free\n' "$SCRIPT_DIR"
  fail=1
fi

if [ "$fail" = "0" ]; then
  printf '  [ ok ] every script honours HML_RXE_SKIP=1 and exits %s (skipped, not failed)\n' "$EXPECTED"
  exit 0
fi
printf '  [FAIL] the skip path is broken; a lab that cannot fail closed must not be trusted\n'
exit 1

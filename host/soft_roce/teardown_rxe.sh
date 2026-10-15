#!/usr/bin/env bash
#
# teardown_rxe.sh -- remove the M3b Soft-RoCE lab. Safe to run twice.
#
# Removes (in order): processes inside each netns, the netns itself (which also
# takes its namespace-local rxe device and veth with it), any rxe device or
# veth that leaked into the root namespace, and the /var/run/netns mount point
# entries we created.
#
# "Nothing to tear down" is a success (exit 0). If lab state exists but the
# script is not root, it fails closed with exit 77 (CTest "skipped").
# HML_RXE_SKIP=1 makes it a no-op that reports the skip path (exit 77).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lab_common.sh
. "$SCRIPT_DIR/lab_common.sh"

for _arg in "$@"; do
  case "$_arg" in
    --mock|--dry-run)
      HML_LAB_MOCK=1
      export HML_LAB_MOCK
      ;;
    --quiet)
      lab_set_quiet 1
      ;;
    --help|-h)
      printf 'usage: %s [--quiet] [--mock|--dry-run]\n' "$0"
      exit 0
      ;;
  esac
done

lab_honour_force_skip "teardown_rxe.sh"

if lab_is_mock; then
  rm -rf "$SCRIPT_DIR/artifacts" 2>/dev/null || true
  lab_say "Mock Soft-RoCE lab torn down."
  exit 0
fi

if ! command -v ip >/dev/null 2>&1; then
  # Nothing can be inspected or removed without iproute2; that is not an error.
  lab_say "iproute2 ('ip') is not installed: no Soft-RoCE lab can exist, nothing to tear down."
  exit 0
fi

# ---- is there anything to do? -------------------------------------------
have_work=0
if netns_exists "$HML_NS_A" || netns_exists "$HML_NS_B"; then
  have_work=1
fi
if link_exists "$HML_VETH_A" || link_exists "$HML_VETH_B"; then
  have_work=1
fi
if command -v rdma >/dev/null 2>&1; then
  if rxe_exists "$HML_RXE_A" || rxe_exists "$HML_RXE_B"; then
    have_work=1
  fi
fi

if [ "$have_work" = "0" ]; then
  lab_say "Nothing to tear down: no $HML_NS_A/$HML_NS_B netns, no $HML_VETH_A/$HML_VETH_B veth, no $HML_RXE_A/$HML_RXE_B rxe device."
  exit 0
fi

require_root "teardown_rxe.sh"

lab_step_q "Tearing down the Soft-RoCE lab"

# ---- 1. processes pinned inside the namespaces ---------------------------
for _ns in "$HML_NS_A" "$HML_NS_B"; do
  if netns_exists "$_ns"; then
    _pids=$(ip netns pids "$_ns" 2>/dev/null || true)
    if [ -n "$_pids" ]; then
      # shellcheck disable=SC2086
      kill $_pids 2>/dev/null || true
      sleep 1
      # shellcheck disable=SC2086
      kill -9 $_pids 2>/dev/null || true
    fi
  fi
done
lab_ok_q "namespace processes terminated"

# ---- 2. rxe devices inside the namespaces (explicit, then implicit) -----
for _pair in "$HML_NS_A:$HML_RXE_A" "$HML_NS_B:$HML_RXE_B"; do
  _ns=${_pair%%:*}
  _dev=${_pair##*:}
  if netns_exists "$_ns"; then
    ip netns exec "$_ns" rdma link del "$_dev" >/dev/null 2>&1 || true
  fi
done

# ---- 3. the namespaces (also destroys their veth and rxe devices) -------
for _ns in "$HML_NS_A" "$HML_NS_B"; do
  if netns_exists "$_ns"; then
    ip netns del "$_ns" >/dev/null 2>&1 || true
  fi
done
lab_ok_q "netns $HML_NS_A / $HML_NS_B removed"

# ---- 4. anything that leaked into the root namespace --------------------
if command -v rdma >/dev/null 2>&1; then
  for _dev in "$HML_RXE_A" "$HML_RXE_B"; do
    if rxe_exists "$_dev"; then
      rdma link del "$_dev" >/dev/null 2>&1 || true
    fi
  done
fi
for _dev in "$HML_VETH_A" "$HML_VETH_B"; do
  if link_exists "$_dev"; then
    ip link del "$_dev" >/dev/null 2>&1 || true
  fi
done
lab_ok_q "root-namespace rxe devices / veths removed"

# ---- 5. final verification (best effort, never fatal) -------------------
_left=0
if netns_exists "$HML_NS_A" || netns_exists "$HML_NS_B"; then
  _left=1
fi
if link_exists "$HML_VETH_A" || link_exists "$HML_VETH_B"; then
  _left=1
fi
if [ "$_left" = "1" ]; then
  lab_warn "something survived the teardown; re-run as root or check for stuck namespaces"
else
  lab_say "Soft-RoCE lab torn down."
fi
exit 0

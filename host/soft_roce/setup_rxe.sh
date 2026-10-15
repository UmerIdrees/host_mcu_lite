#!/usr/bin/env bash
#
# setup_rxe.sh -- bring up the M3b Soft-RoCE (rdma_rxe) lab, idempotently.
#
# Topology (all names overridable via the environment, see lab_common.sh):
#
#   netns hml-a  <-- veth veth-hml-a/eth0 -- 10.200.0.1/24 -- rdma rxe0
#   netns hml-b  <-- veth veth-hml-b/eth0 -- 10.200.0.2/24 -- rdma rxe1
#
# Running it twice is safe: any previous lab state is removed first, then the
# lab is rebuilt from scratch. Fails closed with exit 77 (CTest "skipped") when
# root, iproute2, rdma-core or the rdma_rxe kernel module is unavailable.
# Set HML_RXE_SKIP=1 to force the skip path (CI self-test).
#
# M3b IS A LAB: it checks ibverbs CQ/MR-style plumbing only. It is NOT a
# performance proxy for a ConnectX NIC and says so on exit.

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
    --help|-h)
      printf 'usage: %s [--mock|--dry-run]\n' "$0"
      exit 0
      ;;
  esac
done

lab_honour_force_skip "setup_rxe.sh"

lab_step "M3b Soft-RoCE lab setup (rdma_rxe)"

require_root "setup_rxe.sh"
require_tool modprobe " (kmod)"
require_tool rdma " (rdma-core / rdma-core's 'rdma' CLI)"
require_tool ip " (iproute2)"
require_netns_support
ensure_rxe_module

# ---- mock/dry-run path ---------------------------------------------------
if lab_is_mock; then
  lab_step "Removing any previous lab state (idempotency, mock)"
  "$SCRIPT_DIR/teardown_rxe.sh" --quiet --mock || true

  lab_step "Creating network namespaces $HML_NS_A / $HML_NS_B (mock)"
  lab_ok "netns $HML_NS_A, $HML_NS_B (simulated)"

  lab_step "Creating veth pair $HML_VETH_A <-> $HML_VETH_B (mock)"
  lab_ok "veth pair moved into the namespaces and renamed to $HML_ETH_A/$HML_ETH_B (simulated)"

  lab_step "Assigning $HML_IP_A/$HML_CIDR and $HML_IP_B/$HML_CIDR, links up (mock)"
  lab_ok "addresses assigned, MTU $HML_MTU, lo up in both namespaces (simulated)"

  lab_step "Attaching Soft-RoCE devices $HML_RXE_A / $HML_RXE_B to the veths (mock)"
  lab_ok "$HML_RXE_A bound to $HML_ETH_A in $HML_NS_A, $HML_RXE_B bound to $HML_ETH_B in $HML_NS_B (simulated)"

  lab_step "Verifying IP connectivity across the pair (mock)"
  lab_ok "ping $HML_IP_A -> $HML_IP_B succeeded (simulated)"

  lab_step "ibv_devinfo sees the Soft-RoCE devices (mock)"
  lab_ok "Soft-RoCE devices visible via ibv_devinfo (simulated)"

  mkdir -p "$SCRIPT_DIR/artifacts"
  touch "$SCRIPT_DIR/artifacts/.mock_lab_active"

  lab_step "Lab is up (simulated / mock mode)"
  lab_info "  $HML_NS_A: $HML_IP_A/$HML_CIDR on $HML_ETH_A, RDMA device $HML_RXE_A (mock)"
  lab_info "  $HML_NS_B: $HML_IP_B/$HML_CIDR on $HML_ETH_B, RDMA device $HML_RXE_B (mock)"
  lab_info "  tear down with: $SCRIPT_DIR/teardown_rxe.sh --mock"
  exit 0
fi

# ---- idempotency: drop whatever is left over -----------------------------
lab_step "Removing any previous lab state (idempotency)"
"$SCRIPT_DIR/teardown_rxe.sh" --quiet || true

# ---- namespaces ----------------------------------------------------------
lab_step "Creating network namespaces $HML_NS_A / $HML_NS_B"
ip netns add "$HML_NS_A"
ip netns add "$HML_NS_B"
lab_ok "netns $HML_NS_A, $HML_NS_B"

# ---- veth pair -----------------------------------------------------------
lab_step "Creating veth pair $HML_VETH_A <-> $HML_VETH_B"
ip link add "$HML_VETH_A" type veth peer name "$HML_VETH_B"
ip link set "$HML_VETH_A" netns "$HML_NS_A"
ip link set "$HML_VETH_B" netns "$HML_NS_B"
ip -n "$HML_NS_A" link set "$HML_VETH_A" name "$HML_ETH_A"
ip -n "$HML_NS_B" link set "$HML_VETH_B" name "$HML_ETH_B"
lab_ok "veth pair moved into the namespaces and renamed to $HML_ETH_A/$HML_ETH_B"

# ---- addressing ----------------------------------------------------------
lab_step "Assigning $HML_IP_A/$HML_CIDR and $HML_IP_B/$HML_CIDR, links up"
ip -n "$HML_NS_A" addr add "$HML_IP_A/$HML_CIDR" dev "$HML_ETH_A"
ip -n "$HML_NS_B" addr add "$HML_IP_B/$HML_CIDR" dev "$HML_ETH_B"
ip -n "$HML_NS_A" link set "$HML_ETH_A" mtu "$HML_MTU" up
ip -n "$HML_NS_B" link set "$HML_ETH_B" mtu "$HML_MTU" up
ip -n "$HML_NS_A" link set lo up
ip -n "$HML_NS_B" link set lo up
lab_ok "addresses assigned, MTU $HML_MTU, lo up in both namespaces"

# ---- Soft-RoCE devices ---------------------------------------------------
lab_step "Attaching Soft-RoCE devices $HML_RXE_A / $HML_RXE_B to the veths"
if rxe_exists "$HML_RXE_A"; then
  ip netns exec "$HML_NS_A" rdma link del "$HML_RXE_A" || true
fi
if rxe_exists "$HML_RXE_B"; then
  ip netns exec "$HML_NS_B" rdma link del "$HML_RXE_B" || true
fi
ip netns exec "$HML_NS_A" rdma link add "$HML_RXE_A" type rxe netdev "$HML_ETH_A"
ip netns exec "$HML_NS_B" rdma link add "$HML_RXE_B" type rxe netdev "$HML_ETH_B"
lab_ok "$HML_RXE_A bound to $HML_ETH_A in $HML_NS_A, $HML_RXE_B bound to $HML_ETH_B in $HML_NS_B"

# ---- connectivity sanity -------------------------------------------------
lab_step "Verifying IP connectivity across the pair"
if ping -c 2 -W 2 -I "$HML_ETH_A" "$HML_IP_B" >/dev/null 2>&1; then
  lab_ok "ping $HML_IP_A -> $HML_IP_B succeeded"
else
  lab_warn "ping $HML_IP_A -> $HML_IP_B failed (no iputils-ping, or netfilter dropped it); the RoCE data path may still work"
fi

# ---- device visibility ---------------------------------------------------
if command -v ibv_devinfo >/dev/null 2>&1; then
  lab_step "ibv_devinfo sees the Soft-RoCE devices"
  ip netns exec "$HML_NS_A" ibv_devinfo -d "$HML_RXE_A" || \
    lab_die "ibv_devinfo could not open $HML_RXE_A in $HML_NS_A"
  ip netns exec "$HML_NS_B" ibv_devinfo -d "$HML_RXE_B" || \
    lab_die "ibv_devinfo could not open $HML_RXE_B in $HML_NS_B"
else
  lab_warn "ibv_devinfo not installed: the lab is up but cannot be verified from userspace"
fi

lab_step "Lab is up"
lab_info "  $HML_NS_A: $HML_IP_A/$HML_CIDR on $HML_ETH_A, RDMA device $HML_RXE_A"
lab_info "  $HML_NS_B: $HML_IP_B/$HML_CIDR on $HML_ETH_B, RDMA device $HML_RXE_B"
lab_info "  tear down with: $SCRIPT_DIR/teardown_rxe.sh"
lab_info "  NOTE: Soft-RoCE (rxe) is a software loopback NIC. Results here are NOT a"
lab_info "        performance proxy for a ConnectX NIC or GPU Direct RDMA."
exit 0

#!/usr/bin/env bash
# shellcheck shell=bash
#
# lab_common.sh -- shared helpers for the M3b Soft-RoCE (rdma_rxe) lab.
#
# This file is SOURCED, never executed. It implements the fail-closed skip
# protocol used by every script in this directory:
#
#   * a missing prerequisite prints exactly what is missing, then exits 77;
#   * CTest maps 77 to "Skipped" (SKIP_RETURN_CODE), never to a failure;
#   * HML_RXE_SKIP=1 forces the skip path so CI can self-test without root,
#     without the rdma_rxe module and without RDMA hardware.
#
# POSIX-bash only: [ ] tests, quoted expansions, no arrays, no [[ ]], no
# process substitution (the one concession is `set -o pipefail`).

# CTest "skipped" return code.
HML_LAB_SKIP_RC=77

# ---- lab topology (override via env if you need a second lab on one box) ----
HML_NS_A=${HML_NS_A:-hml-a}
HML_NS_B=${HML_NS_B:-hml-b}
HML_VETH_A=${HML_VETH_A:-veth-hml-a}
HML_VETH_B=${HML_VETH_B:-veth-hml-b}
HML_ETH_A=${HML_ETH_A:-eth0}
HML_ETH_B=${HML_ETH_B:-eth0}
HML_RXE_A=${HML_RXE_A:-rxe0}
HML_RXE_B=${HML_RXE_B:-rxe1}
HML_IP_A=${HML_IP_A:-10.200.0.1}
HML_IP_B=${HML_IP_B:-10.200.0.2}
HML_CIDR=${HML_CIDR:-24}
HML_MTU=${HML_MTU:-1500}

# ---- output helpers -------------------------------------------------------
lab_step() { printf '\n== %s ==\n' "$*"; }
lab_info() { printf '       %s\n' "$*"; }
lab_ok()   { printf '  [ ok ] %s\n' "$*"; }
lab_warn() { printf '  [warn] %s\n' "$*" >&2; }
lab_err()  { printf '  [fail] %s\n' "$*" >&2; }

# lab_skip <message...> -- the single exit-77 funnel.
lab_skip() {
  printf '  [skip] %s\n' "$*" >&2
  printf '  [skip] M3b Soft-RoCE lab is OPTIONAL; failing closed, CI continues.\n' >&2
  exit "$HML_LAB_SKIP_RC"
}

# lab_die <message...> -- a real failure (exit 1), used only for "we were able
# to start the lab and it then misbehaved".
lab_die() {
  printf '  [fail] %s\n' "$*" >&2
  exit 1
}

lab_quiet=0
lab_set_quiet() { if [ "$1" = "1" ]; then lab_quiet=1; fi; }
lab_say() { if [ "$lab_quiet" = "0" ]; then printf '%s\n' "$*"; fi; }
lab_step_q() { if [ "$lab_quiet" = "0" ]; then lab_step "$*"; fi; }
lab_ok_q() { if [ "$lab_quiet" = "0" ]; then lab_ok "$*"; fi; }
lab_info_q() { if [ "$lab_quiet" = "0" ]; then lab_info "$*"; fi; }

# ---- forced skip ---------------------------------------------------------
# HML_RXE_SKIP=1 short-circuits every privileged step. Used by CTest test
# roce_lab_skip_path to prove the skip path itself works.
lab_honour_force_skip() {
  if [ "${HML_RXE_SKIP:-0}" = "1" ]; then
    lab_skip "HML_RXE_SKIP=1 is set: ${1:-script} performed no changes on purpose (CI self-test of the skip path)."
  fi
}

# ---- requirement helpers -------------------------------------------------
lab_is_root() { [ "$(id -u)" = "0" ]; }
lab_is_mock() { [ "${HML_LAB_MOCK:-0}" = "1" ] || [ "${HML_LAB_DRY_RUN:-0}" = "1" ]; }

# require_root <script-name>
require_root() {
  if lab_is_mock; then
    lab_ok "root privileges (mock/dry-run mode)"
    return 0
  fi
  if ! lab_is_root; then
    lab_skip "${1:-this script} needs root (uid 0) for netns/veth/rdma operations; current uid is $(id -u). Re-run with sudo, or leave the lab unbuilt (it is optional)."
  fi
}

# require_tool <tool> [hint]
require_tool() {
  _lab_t=$1
  _lab_hint=${2:-}
  if lab_is_mock; then
    lab_ok "required tool '$_lab_t' (mock/dry-run mode)"
    return 0
  fi
  if ! command -v "$_lab_t" >/dev/null 2>&1; then
    lab_skip "required tool '$_lab_t' not found in PATH. Missing: '$_lab_t'.${_lab_hint}"
  fi
}

# module_present <name> -- read-only: already loaded, or loadable via modinfo.
module_present() {
  _lab_m=$1
  if lab_is_mock; then
    return 0
  fi
  if [ -d "/sys/module/$_lab_m" ]; then
    return 0
  fi
  if command -v modinfo >/dev/null 2>&1 && modinfo "$_lab_m" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

# require_module <name> [hint] -- read-only; never loads anything.
require_module() {
  _lab_m=$1
  _lab_hint=${2:-}
  if module_present "$_lab_m"; then
    return 0
  fi
  lab_skip "kernel module '$_lab_m' is not available (not loaded and not present in the module tree). Missing: '$_lab_m'.${_lab_hint}"
}

# ensure_rxe_module -- attempt modprobe once, then fail closed with the exact
# kernel-configuration hint. Only call this from root, privileged scripts.
ensure_rxe_module() {
  if lab_is_mock; then
    lab_ok "rdma_rxe present (mock/dry-run mode)"
    return 0
  fi
  if module_present rdma_rxe; then
    lab_ok "rdma_rxe present"
    return 0
  fi
  if ! command -v modprobe >/dev/null 2>&1; then
    lab_skip "rdma_rxe is not loaded and 'modprobe' is missing. Missing: kmod. Install kmod (Debian/Ubuntu) or kmod (Fedora/RHEL)."
  fi
  if modprobe rdma_rxe >/dev/null 2>&1; then
    lab_ok "rdma_rxe loaded via modprobe"
    return 0
  fi
  lab_skip "modprobe rdma_rxe failed. Missing: the rdma_rxe kernel module (CONFIG_RDMA_RXE=m). Install modules-extra / kernel-modules-extra for your kernel, or rdma-core's rxe driver package."
}

# require_netns_support -- iproute2 with network-namespace support.
require_netns_support() {
  if lab_is_mock; then
    lab_ok "netns support present (mock/dry-run mode)"
    return 0
  fi
  require_tool ip " (iproute2 package)"
  if ! ip netns list >/dev/null 2>&1; then
    lab_skip "'ip netns list' failed: this iproute2 has no usable netns support (needs /var/run/netns and CONFIG_NET_NS). Missing: functional network namespaces."
  fi
  if [ ! -d /var/run/netns ] && ! mkdir -p /var/run/netns 2>/dev/null; then
    lab_skip "cannot create /var/run/netns; the mount point for named network namespaces is missing. Missing: writable /var/run/netns."
  fi
}

# ---- state probes (read-only) --------------------------------------------
netns_exists() {
  if lab_is_mock; then
    [ -f "${SCRIPT_DIR:-.}/artifacts/.mock_lab_active" ] && return 0
    return 1
  fi
  ip netns list 2>/dev/null | cut -d' ' -f1 | grep -qx "$1"
}

link_exists() {
  if lab_is_mock; then
    [ -f "${SCRIPT_DIR:-.}/artifacts/.mock_lab_active" ] && return 0
    return 1
  fi
  ip link show "$1" >/dev/null 2>&1
}

rxe_exists() {
  if lab_is_mock; then
    [ -f "${SCRIPT_DIR:-.}/artifacts/.mock_lab_active" ] && return 0
    return 1
  fi
  rdma link show "$1" >/dev/null 2>&1
}

rxe_netdev() {
  if lab_is_mock; then
    case "$1" in
      "$HML_RXE_A") printf '%s\n' "$HML_ETH_A" ;;
      "$HML_RXE_B") printf '%s\n' "$HML_ETH_B" ;;
      *) printf 'eth0\n' ;;
    esac
    return 0
  fi
  rdma link show "$1" 2>/dev/null | sed -n 's/.*netdev \([^ ]*\).*/\1/p' | head -n 1
}

lab_is_up() { netns_exists "$HML_NS_A" && netns_exists "$HML_NS_B"; }

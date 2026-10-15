#!/usr/bin/env bash
#
# setup_tap_soft_roce.sh -- Configure Linux Kernel Soft-RoCE (rxe0) over TAP interface.
#
# This script sets up a Linux TAP interface (tap0) with IP 10.0.0.2/24 and binds
# the kernel rdma_rxe driver (rxe0) to it, allowing Renode MCU emulations to send
# real RoCEv2 datagrams directly into the Linux kernel libibverbs subsystem.
#
# Usage:
#   sudo ./setup_tap_soft_roce.sh [--teardown]
#

set -euo pipefail

TAP_DEV="tap0"
RXE_DEV="rxe0"
HOST_IP="10.0.0.2/24"

if [ "$(id -u)" -ne 0 ]; then
  echo "Error: This script requires root privileges. Please run with sudo." >&2
  exit 1
fi

teardown() {
  echo "[Teardown] Removing Soft-RoCE device ${RXE_DEV} and TAP interface ${TAP_DEV}..."
  if rdma link show "${RXE_DEV}" >/dev/null 2>&1; then
    rdma link del "${RXE_DEV}" || true
    echo "  - Removed rdma link ${RXE_DEV}"
  fi
  if ip link show "${TAP_DEV}" >/dev/null 2>&1; then
    ip link del "${TAP_DEV}" || true
    echo "  - Removed network interface ${TAP_DEV}"
  fi
  echo "[Teardown] Complete."
}

if [ "${1:-}" = "--teardown" ] || [ "${1:-}" = "-d" ]; then
  teardown
  exit 0
fi

echo "======================================================================"
echo " Setting up Linux Kernel Soft-RoCE (${RXE_DEV}) over TAP (${TAP_DEV})"
echo "======================================================================"

# Clean up any stale configuration first
teardown >/dev/null 2>&1 || true

# 1. Load the kernel Soft-RoCE module
echo "[1/4] Loading rdma_rxe kernel module..."
modprobe rdma_rxe || {
  echo "Error: Failed to load rdma_rxe module. Ensure linux-modules-extra is installed." >&2
  exit 1
}

# 2. Create the TAP interface with ownership assigned to the calling user
echo "[2/4] Creating TAP interface ${TAP_DEV} (user: ${SUDO_USER:-$USER})..."
ip tuntap add mode tap dev "${TAP_DEV}" user "${SUDO_USER:-$USER}"
ip addr add "${HOST_IP}" dev "${TAP_DEV}"
ip link set "${TAP_DEV}" up

# 3. Attach Soft-RoCE to the TAP interface
echo "[3/4] Binding Soft-RoCE device ${RXE_DEV} to ${TAP_DEV}..."
rdma link add "${RXE_DEV}" type rxe netdev "${TAP_DEV}"

# 4. Verification
echo "[4/4] Verifying Soft-RoCE device status..."
if command -v ibv_devinfo >/dev/null 2>&1; then
  ibv_devinfo -d "${RXE_DEV}" | grep -E "hca_id|transport|state|link_layer" || true
else
  rdma dev show "${RXE_DEV}"
  rdma link show
fi

echo "======================================================================"
echo " Soft-RoCE is READY!"
echo "   Device   : ${RXE_DEV}"
echo "   Interface: ${TAP_DEV} (${HOST_IP})"
echo "   To remove: sudo $0 --teardown"
echo "======================================================================"

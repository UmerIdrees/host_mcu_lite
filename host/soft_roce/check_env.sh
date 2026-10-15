#!/usr/bin/env bash
#
# check_env.sh -- report whether the M3b Soft-RoCE (rdma_rxe) lab can run here.
#
# Read-only: it never creates, loads or removes anything. Prints a report and
#   exit 0  -> the lab is runnable (every REQUIRED item is present)
#   exit 77 -> not runnable; the report names exactly what is missing
#             (CTest maps 77 to "Skipped", never to a failure)
#   exit 1  -> the report itself could not be produced
#
# HML_RXE_SKIP=1 forces exit 77 (CI self-test of the skip path).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lab_common.sh
. "$SCRIPT_DIR/lab_common.sh"

lab_honour_force_skip "check_env.sh"

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

printf '=== M3b Soft-RoCE (rdma_rxe) lab environment report ===\n'
printf 'date : %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
printf 'host : %s\n' "$(uname -n)"
printf 'kern : %s\n' "$(uname -r)"
printf 'dist : %s\n' "$( ( . /etc/os-release 2>/dev/null && printf '%s' "${PRETTY_NAME:-unknown}" ) || printf 'unknown')"
printf 'user : uid=%s (%s)\n' "$(id -u)" "$(id -un 2>/dev/null || printf '?')"
printf 'lab  : %s <-> %s (%s/%s, %s <-> %s)\n' \
  "$HML_NS_A" "$HML_NS_B" "$HML_IP_A" "$HML_IP_B" "$HML_RXE_A" "$HML_RXE_B"
if lab_is_mock; then
  printf 'mode : simulated / mock verification mode (unprivileged non-root CI)\n'
fi
printf '\n--- REQUIRED ---\n'

missing=0

# note_missing <hint> -- records a remediation hint; the count is printed in
# the verdict, so a hint is never mistaken for a second failed check.
note_missing() {
  missing=$((missing + 1))
  printf '         fix: %s\n' "$1"
}

# 1. root ------------------------------------------------------------------
if lab_is_root || lab_is_mock; then
  if lab_is_mock; then
    printf '  [ ok ] root privileges (simulated / mock mode)\n'
  else
    printf '  [ ok ] root privileges (uid 0) -- netns/veth/rdma operations allowed\n'
  fi
else
  printf '  [MISS] root privileges: uid=%s. netns/veth/rdma link operations need root\n' "$(id -u)"
  note_missing "run as root or with sudo (only for the real lab; the library and its tests need none of this)"
fi

# 2. netns tooling ---------------------------------------------------------
if command -v ip >/dev/null 2>&1 || lab_is_mock; then
  if lab_is_mock; then
    printf '  [ ok ] iproute2 with working netns support (simulated)\n'
  elif ip netns list >/dev/null 2>&1; then
    printf '  [ ok ] iproute2 (%s) with working netns support\n' "$(command -v ip)"
  else
    printf '  [MISS] %s exists but "ip netns list" fails: no usable netns support\n' "$(command -v ip)"
    note_missing "functional network namespaces (CONFIG_NET_NS, writable /var/run/netns)"
  fi
else
  printf '  [MISS] "ip" (iproute2) not in PATH\n'
  note_missing "iproute2"
fi

# 3. rdma_rxe module -------------------------------------------------------
if module_present rdma_rxe || lab_is_mock; then
  if lab_is_mock; then
    printf '  [ ok ] kernel module rdma_rxe is available (simulated)\n'
  elif [ -d /sys/module/rdma_rxe ]; then
    printf '  [ ok ] kernel module rdma_rxe is loaded\n'
  else
    printf '  [ ok ] kernel module rdma_rxe is available (not loaded yet; setup_rxe.sh modprobes it)\n'
  fi
else
  printf '  [MISS] kernel module rdma_rxe not loaded and not found in the module tree\n'
  note_missing "rdma_rxe (CONFIG_RDMA_RXE=m): install modules-extra / kernel-modules-extra for kernel $(uname -r)"
fi

# 4. rdma-core user space --------------------------------------------------
if command -v rdma >/dev/null 2>&1 || lab_is_mock; then
  if lab_is_mock; then
    printf '  [ ok ] rdma-core CLI (simulated)\n'
  else
    printf '  [ ok ] rdma-core CLI (%s) -- "rdma link add ... type rxe" available\n' "$(command -v rdma)"
  fi
else
  printf '  [MISS] "rdma" CLI not in PATH\n'
  note_missing "rdma-core (Debian/Ubuntu/Fedora package: rdma-core)"
fi

if command -v modprobe >/dev/null 2>&1 || lab_is_mock; then
  if lab_is_mock; then
    printf '  [ ok ] modprobe (simulated)\n'
  else
    printf '  [ ok ] modprobe (%s)\n' "$(command -v modprobe)"
  fi
else
  printf '  [MISS] "modprobe" not in PATH (needed to load rdma_rxe)\n'
  note_missing "kmod"
fi

# 5. ibverbs user space ----------------------------------------------------
if command -v ibv_devinfo >/dev/null 2>&1 || lab_is_mock; then
  if lab_is_mock; then
    printf '  [ ok ] ibv_devinfo (simulated ibverbs user space)\n'
  else
    printf '  [ ok ] ibv_devinfo (%s) -- ibverbs user space available\n' "$(command -v ibv_devinfo)"
  fi
else
  printf '  [MISS] ibv_devinfo not in PATH (ibverbs user-space tools)\n'
  note_missing "ibverbs user space: rdma-core / libibverbs-utils (provides ibv_devinfo; package name varies per distro)"
fi

# --- OPTIONAL -------------------------------------------------------------
printf '\n--- OPTIONAL (improves the lab report, never required) ---\n'
for _t in ibstat perftest modinfo ping timeout; do
  if command -v "$_t" >/dev/null 2>&1; then
    printf '  [ ok ] %-12s %s\n' "$_t" "$(command -v "$_t")"
  else
    printf '  [   ] %-12s missing (optional)\n' "$_t"
  fi
done

printf '\n--- CURRENT LAB STATE (read-only probe) ---\n'
if command -v ip >/dev/null 2>&1; then
  if netns_exists "$HML_NS_A" || netns_exists "$HML_NS_B"; then
    printf '  netns %s / %s: present\n' "$HML_NS_A" "$HML_NS_B"
  else
    printf '  netns %s / %s: absent (normal; run setup_rxe.sh)\n' "$HML_NS_A" "$HML_NS_B"
  fi
  for _l in "$HML_VETH_A" "$HML_VETH_B"; do
    if link_exists "$_l"; then
      printf '  link  %s: present in the root namespace\n' "$_l"
    else
      printf '  link  %s: absent\n' "$_l"
    fi
  done
fi
if command -v rdma >/dev/null 2>&1; then
  for _d in "$HML_RXE_A" "$HML_RXE_B"; do
    if rxe_exists "$_d"; then
      printf '  rxe   %s: present (netdev %s)\n' "$_d" "$(rxe_netdev "$_d")"
    else
      printf '  rxe   %s: absent\n' "$_d"
    fi
  done
fi
if command -v ibv_devinfo >/dev/null 2>&1; then
  _devs=$(ibv_devinfo -l 2>/dev/null || true)
  if [ -n "$_devs" ]; then
    printf '  ibv_devinfo -l in the root namespace:\n'
    printf '%s\n' "$_devs" | sed 's/^/      /'
    if printf '%s\n' "$_devs" | grep -q '^rxe'; then
      printf '  rxe devices already listed above (the lab is up in the root namespace)\n'
    else
      printf '  no rxe device listed here: expected until setup_rxe.sh runs\n'
    fi
  else
    printf '  ibv_devinfo -l: no RDMA devices in the root namespace (expected on a bare lab box)\n'
  fi
fi

printf '\n--- M3 SOCKET-RoCE BUILD ARTIFACTS (informational) ---\n'
_found=0
for _d in "$SCRIPT_DIR/../../build" "$SCRIPT_DIR/../build" "$SCRIPT_DIR/../build" \
          /tmp/opencode/build /tmp/opencode/build-release /tmp/opencode/build-d; do
  for _b in hml_gen_roce hml_host_roce; do
    if [ -x "$_d/host/roce_demux/$_b" ] || [ -x "$_d/$_b" ]; then
      printf '  [ ok ] %s found under %s\n' "$_b" "$_d"
      _found=1
    fi
  done
done
if [ "$_found" = "0" ]; then
  printf '  [   ] no hml_gen_roce / hml_host_roce binary found; run_lab.sh will skip the\n'
  printf '         M3 socket-RoCE loopback sanity pair (that pair is optional here)\n'
fi

printf '\n--- VERDICT ---\n'
if [ "$missing" = "0" ]; then
  if lab_is_mock; then
    printf '  RUNNABLE (mock / dry-run): simulated Soft-RoCE pipeline verification mode.\n'
  else
    printf '  RUNNABLE: the Soft-RoCE lab can be brought up (see run_lab.sh).\n'
  fi
  printf '  Reminder: rxe is a software NIC -- this lab is an ibverbs CQ/MR correctness\n'
  printf '  lab, NOT a performance proxy for ConnectX or GPU Direct.\n'
  exit 0
fi

printf '  NOT RUNNABLE: %s required item(s) missing (see the "fix:" lines above).\n' "$missing"
exit "$HML_LAB_SKIP_RC"

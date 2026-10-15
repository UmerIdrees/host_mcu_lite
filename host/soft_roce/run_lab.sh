#!/usr/bin/env bash
#
# run_lab.sh -- M3b Soft-RoCE (rdma_rxe) lab driver.
#
#   (a) run check_env.sh                       (read-only environment report)
#   (b) bring the lab up                        (setup_rxe.sh, idempotent)
#   (c) verify ibv_devinfo shows rxe0/rxe1 and dump their GIDs
#   (d) save short logs under artifacts/        (git-ignored, created on demand)
#   (e) run the M3 socket-RoCE sanity pair over loopback if the binaries exist
#   (f) ALWAYS tear the lab down again (EXIT trap)
#   (g) state the non-claim: Soft-RoCE is not a ConnectX performance proxy
#
# Exit codes: 0 = lab ran (or ran as far as this box allows, with the
# environment-dependent parts explicitly reported as skipped), 77 = skipped
# because the environment lacks a prerequisite (CTest "Skipped", not a failure).
#
# Flags: --keep (leave the lab up, for manual poking), --help.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lab_common.sh
. "$SCRIPT_DIR/lab_common.sh"

ARTIFACTS="$SCRIPT_DIR/artifacts"
STAMP=$(date -u '+%Y%m%dT%H%M%SZ')

for _arg in "$@"; do
  case "$_arg" in
    --keep) HML_LAB_KEEP=1 ;;
    --mock|--dry-run)
      HML_LAB_MOCK=1
      export HML_LAB_MOCK
      ;;
    --help|-h)
      printf 'usage: %s [--keep] [--mock|--dry-run]\n' "$0"
      exit 0
      ;;
    *)
      printf 'usage: %s [--keep] [--mock|--dry-run]\n' "$0" >&2
      exit 1
      ;;
  esac
done
: "${HML_LAB_KEEP:=0}"

# Forced-skip check comes before the teardown trap is installed: with
# HML_RXE_SKIP=1 this script must have no side effects whatsoever.
lab_honour_force_skip "run_lab.sh"

# ---- (f) teardown trap, always ------------------------------------------
# shellcheck disable=SC2329  # invoked by the EXIT trap below
on_exit() {
  _rc=$?
  trap - EXIT
  if [ "${HML_LAB_KEEP}" = "1" ]; then
    printf '\n[HML_LAB_KEEP=1] leaving the lab up; remove it with: %s/teardown_rxe.sh\n' "$SCRIPT_DIR"
  else
    printf '\n== teardown (via EXIT trap) ==\n'
    if lab_is_mock; then
      HML_RXE_SKIP='' "$SCRIPT_DIR/teardown_rxe.sh" --mock || true
      rm -rf "$ARTIFACTS" 2>/dev/null || true
    else
      HML_RXE_SKIP='' "$SCRIPT_DIR/teardown_rxe.sh" || true
    fi
  fi
  exit "$_rc"
}
trap on_exit EXIT
trap 'exit 130' INT TERM

lab_step "M3b Soft-RoCE (rdma_rxe) lab -- driver"
printf 'started %s (root=%s, HML_RXE_SKIP=%s, mock=%s)\n' "$STAMP" "$(id -u)" "${HML_RXE_SKIP:-0}" "${HML_LAB_MOCK:-0}"

# ---- (a) environment report ---------------------------------------------
lab_step "(a) Environment report (check_env.sh)"
set +e
if lab_is_mock; then
  "$SCRIPT_DIR/check_env.sh" --mock
else
  "$SCRIPT_DIR/check_env.sh"
fi
_env_rc=$?
set -e
if [ "$_env_rc" = "$HML_LAB_SKIP_RC" ]; then
  lab_skip "the environment cannot run the Soft-RoCE lab (see the check_env.sh report above)."
elif [ "$_env_rc" != "0" ]; then
  lab_die "check_env.sh failed with exit code $_env_rc"
fi
lab_ok "environment is lab-runnable"

# ---- (b) bring the lab up -----------------------------------------------
lab_step "(b) Bringing the lab up (setup_rxe.sh)"
if lab_is_mock; then
  "$SCRIPT_DIR/setup_rxe.sh" --mock
else
  "$SCRIPT_DIR/setup_rxe.sh"
fi

# ---- (d) artifacts directory (created on demand, git-ignored) -----------
mkdir -p "$ARTIFACTS"
lab_ok "artifacts directory: $ARTIFACTS (git-ignored)"

# ---- (c) verify the rxe devices and their GIDs --------------------------
lab_step "(c) Verifying Soft-RoCE devices with ibv_devinfo"
if ! lab_is_mock && ! command -v ibv_devinfo >/dev/null 2>&1; then
  lab_skip "ibv_devinfo disappeared between check_env.sh and the verification step. Missing: ibverbs user-space tools."
fi

verify_dev() {
  _ns=$1
  _dev=$2
  _log="$ARTIFACTS/ibv_devinfo-$_ns-$STAMP.txt"
  if lab_is_mock; then
    cat <<EOF >"$_log"
hca_id: $_dev
	transport:			InfiniBand (0)
	fw_ver:				1.0.0
	node_guid:			0011:2233:4455:6677
	sys_image_guid:			0011:2233:4455:6677
	vendor_id:			0x0000
	vendor_part_id:			0
	hw_ver:				0x0
	phys_port_cnt:			1
		port:	1
			state:			PORT_ACTIVE (4)
			max_mtu:		4096 (5)
			active_mtu:		1024 (3)
			sm_lid:			0
			port_lid:		0
			port_lmc:		0x00
			link_layer:		Ethernet
			GID[ 0]:		fe80:0000:0000:0000:0211:22ff:fe33:4455
EOF
  elif ! ip netns exec "$_ns" ibv_devinfo -d "$_dev" >"$_log" 2>&1; then
    printf '\n' >>"$_log"
    printf '  [fail] ibv_devinfo -d %s inside netns %s failed; log: %s\n' "$_dev" "$_ns" "$_log" >&2
    sed 's/^/      /' "$_log" >&2
    return 1
  fi
  if ! grep -q "$_dev" "$_log"; then
    printf '  [fail] ibv_devinfo did not list %s inside %s; log: %s\n' "$_dev" "$_ns" "$_log" >&2
    return 1
  fi
  lab_ok "$_dev visible in netns $_ns (log: $(basename "$_log"))"
  if grep -q 'GID' "$_log"; then
    lab_info "GID as reported by ibv_devinfo:"
    grep -E 'GID|port_lid|port_sm' "$_log" | sed 's/^/      /'
  else
    lab_warn "no GID line in the ibv_devinfo output for $_dev"
  fi
  return 0
}

verify_dev "$HML_NS_A" "$HML_RXE_A"
verify_dev "$HML_NS_B" "$HML_RXE_B"

# ---- (d) dump ibstat / rdma link state when available ------------------
lab_step "(d) Saving a short state dump to artifacts/"
{
  printf '# M3b Soft-RoCE lab state dump  %s\n' "$STAMP"
  printf '# namespaces: %s (%s), %s (%s)\n' "$HML_NS_A" "$HML_IP_A" "$HML_NS_B" "$HML_IP_B"
  printf '# rxe: %s on %s, %s on %s\n' "$HML_RXE_A" "$HML_ETH_A" "$HML_RXE_B" "$HML_ETH_B"
  printf '\n$ ip -n %s addr\n' "$HML_NS_A"; ip -n "$HML_NS_A" addr 2>&1 || true
  printf '\n$ ip -n %s addr\n' "$HML_NS_B"; ip -n "$HML_NS_B" addr 2>&1 || true
  printf '\n$ ip netns exec %s rdma link show\n' "$HML_NS_A"
  ip netns exec "$HML_NS_A" rdma link show 2>&1 || true
  printf '\n$ ip netns exec %s rdma link show\n' "$HML_NS_B"
  ip netns exec "$HML_NS_B" rdma link show 2>&1 || true
} >"$ARTIFACTS/lab-state-$STAMP.txt" 2>&1
lab_ok "lab state: $(basename "$ARTIFACTS/lab-state-$STAMP.txt")"

if command -v ibstat >/dev/null 2>&1; then
  for _pair in "$HML_NS_A:$HML_RXE_A" "$HML_NS_B:$HML_RXE_B"; do
    _ns=${_pair%%:*}
    _dev=${_pair##*:}
    ip netns exec "$_ns" ibstat -d "$_dev" >"$ARTIFACTS/ibstat-$_ns-$STAMP.txt" 2>&1 || true
  done
  lab_ok "ibstat dumps: artifacts/ibstat-$HML_NS_A-$STAMP.txt, artifacts/ibstat-$HML_NS_B-$STAMP.txt"
else
  lab_info "ibstat not installed: skipped the per-device state dump (optional)"
fi

# ---- (e) M3 socket-RoCE sanity pair over loopback ----------------------
lab_step "(e) M3 socket-RoCE (T1a) loopback sanity pair, if the binaries exist"
_rx_bin=""
_tx_bin=""
for _d in "$SCRIPT_DIR/../../build" "$SCRIPT_DIR/../build" \
          /tmp/opencode/build /tmp/opencode/build-release /tmp/opencode/build-d; do
  if [ -z "$_rx_bin" ]; then
    for _c in "$_d/host/roce_demux/hml_host_roce" "$_d/hml_host_roce"; do
      if [ -x "$_c" ]; then _rx_bin=$_c; break; fi
    done
  fi
  if [ -z "$_tx_bin" ]; then
    for _c in "$_d/host/roce_demux/hml_gen_roce" "$_d/hml_gen_roce"; do
      if [ -x "$_c" ]; then _tx_bin=$_c; break; fi
    done
  fi
done

if [ -z "$_tx_bin" ]; then
  lab_info "SKIPPED (not a failure): no hml_gen_roce emitter CLI is built."
  if [ -n "$_rx_bin" ]; then
    lab_info "  the receiver CLI is present ($_rx_bin) but there is no built emitter"
    lab_info "  to pair it with, so the loopback exchange was not attempted."
  else
    lab_info "  neither hml_gen_roce nor hml_host_roce is built."
  fi
  lab_info "  Build first (cmake --build <build>), or cover M3/T1a on loopback with the"
  lab_info "  in-tree CTest tests: ctest -R roce  (tests/m3/test_roce_e2e drives the"
  lab_info "  same loopback sanity pair without any RDMA environment)."
else
  lab_info "receiver: $_rx_bin"
  lab_info "emitter : $_tx_bin"
  _rxlog="$ARTIFACTS/m3-loopback-rx-$STAMP.txt"
  _txlog="$ARTIFACTS/m3-loopback-tx-$STAMP.txt"
  # Defaults mirror host/roce_demux/hml_host_roce.c: port 14791 (unprivileged
  # mirror of HML_ROCE_PORT 4791), window_base 0x100000, window_len 65536,
  # frame_size 9000, rkey 0xDEADBEEF.
  if [ -z "$_rx_bin" ]; then
    lab_info "SKIPPED: hml_gen_roce exists but hml_host_roce does not; cannot run the pair."
  else
    "$_rx_bin" --port 14791 --window-base 0x100000 --window-len 65536 \
               --frame-size 9000 --rkey 0xDEADBEEF --frames 1 --verbose \
               >"$_rxlog" 2>&1 &
    _rx_pid=$!
    sleep 1
    if kill -0 "$_rx_pid" 2>/dev/null; then
      set +e
      timeout 20 "$_tx_bin" --port 14791 --dest 127.0.0.1 --frames 1 \
            >"$_txlog" 2>&1
      _tx_rc=$?
      set -e
      wait "$_rx_pid" 2>/dev/null || true
      if [ "$_tx_rc" = "124" ]; then
        lab_warn "the M3 emitter timed out after 20s; see $(basename "$_txlog")"
      elif [ "$_tx_rc" != "0" ]; then
        lab_warn "the M3 emitter exited $_tx_rc; see $(basename "$_txlog") (informational: not an rxe result)"
      elif grep -q 'frame seq=' "$_rxlog" 2>/dev/null; then
        lab_ok "M3 loopback pair exchanged a frame; log: $(basename "$_rxlog")"
      else
        lab_warn "the M3 pair did not log a completed frame; see $(basename "$_rxlog")"
      fi
    else
      wait "$_rx_pid" 2>/dev/null || true
      lab_warn "the M3 receiver CLI exited before the emitter ran; see $(basename "$_rxlog")"
    fi
  fi
fi

# ---- (g) the explicit non-claim -----------------------------------------
lab_step "(g) Conclusion"
printf '\n  Soft-RoCE (rdma_rxe) is a SOFTWARE NIC running in the kernel on a veth.\n'
printf '  This lab validates ibverbs-level plumbing (device open, QP/CQ setup, MR\n'
printf '  registration, completion delivery) and nothing else.\n'
printf '\n  It is NOT a performance proxy for a ConnectX NIC and NOT a prediction of\n'
printf '  GPU Direct RDMA throughput or latency. No timing number produced here may be\n'
printf '  quoted as a hardware claim. T1a socket-crafted RoCEv2 (M3) and any real HW\n'
printf '  RoCE / GPU Direct path (M6) are the measured claims.\n'
printf '\n  Lab artifacts: %s\n' "$ARTIFACTS"
exit 0

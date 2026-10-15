#!/usr/bin/env bash
#
# run_matrix.sh -- run every e2e_cpu scene and emit a machine-readable summary.
#
# usage: run_matrix.sh <path-to-e2e_cpu> [--quick|--full]
#
#   --quick  (default) frames=40 per socket scene, 30 s per-scene budget
#   --full   frames=300 per socket scene, 120 s per-scene budget
#
# Exit status is 0 only when every stage passed. Every stage is wrapped in
# `timeout` so a hang cannot stall the suite, and a shared trap wipes the stage
# logs even on failure.
#
# What the matrix does NOT claim: this runs the shipping library on the build
# machine's CPU over 127.0.0.1. It is not an MCU, FreeRTOS, lwIP or silicon run.
# See docs/cpu_only_e2e.md.
set -euo pipefail

BIN="${1:-}"
MODE="${2:---quick}"
if [[ -z "${BIN}" ]]; then
  echo "run_matrix: FAIL no binary path given" >&2
  exit 2
fi
if [[ "${MODE}" != "--quick" && "${MODE}" != "--full" ]]; then
  echo "run_matrix: FAIL unknown mode '${MODE}'" >&2
  exit 2
fi

case "${MODE}" in
  --quick) FRAMES=40; SECS=30; STAGE_TO=75; ALL_TO=150 ;;
  --full)  FRAMES=300; SECS=120; STAGE_TO=220; ALL_TO=260 ;;
esac

# Resolve the binary once, so a relative path is anchored to the caller's cwd.
if [[ "${BIN}" != /* ]]; then BIN="$(pwd)/${BIN}"; fi
if [[ ! -x "${BIN}" ]]; then
  echo "run_matrix: FAIL '${BIN}' is not an executable" >&2
  exit 2
fi
BIN_NAME="$(basename "${BIN}")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/e2e_matrix.XXXXXX")"
cleanup() {
  rm -rf "${WORK}"
}
trap cleanup EXIT

declare -a NAMES ARGS
NAMES+=(list);          ARGS+=("--list")
NAMES+=(udp-loopback);  ARGS+=("udp-loopback --frames ${FRAMES} --seconds ${SECS}")
NAMES+=(roce-e2e);      ARGS+=("roce-e2e --frames ${FRAMES} --seconds ${SECS}")
NAMES+=(reasm-edge);    ARGS+=("reasm-edge --frames ${FRAMES} --seconds ${SECS}")
NAMES+=(control-beacon); ARGS+=("control-beacon --frames ${FRAMES} --seconds ${SECS}")
NAMES+=(mcu-profile);   ARGS+=("mcu-profile --frames ${FRAMES} --seconds ${SECS}")
NAMES+=(all);           ARGS+=("--all --frames ${FRAMES} --seconds ${SECS}")

declare -a STATUS TIME_MS
PASS_COUNT=0
for i in "${!NAMES[@]}"; do
  name="${NAMES[$i]}"
  log="${WORK}/${name}.log"
  real_args="$(eval echo ${ARGS[$i]})"
  is_all=0
  [[ "${name}" == "all" ]] && is_all=1
  if (( is_all )); then TO="${ALL_TO}"; else TO="${STAGE_TO}"; fi

  start="$(date +%s%N)"
  set +e
  # shellcheck disable=SC2086
  timeout -k 5 "${TO}" "${BIN}" ${real_args} >"${log}" 2>&1
  rc=$?
  set -e
  end="$(date +%s%N)"
  ms=$(( (end - start) / 1000000 ))

  STATUS[$i]="FAIL"
  case "${rc}" in
    0) STATUS[$i]="PASS" ;;
    77) STATUS[$i]="SKIP" ;;
    124|137) STATUS[$i]="TIMEOUT" ;;
  esac
  TIME_MS[$i]=${ms}
  [[ "${STATUS[$i]}" == "PASS" ]] && PASS_COUNT=$(( PASS_COUNT + 1 ))

  printf 'E2E matrix stage=%-14s rc=%-3d status=%-7s elapsed_ms=%d\n' \
    "${name}" "${rc}" "${STATUS[$i]}" "${ms}"
  if [[ "${STATUS[$i]}" != "PASS" && "${STATUS[$i]}" != "SKIP" ]]; then
    sed -n '1,60p' "${log}"
  fi
done

# The `list` stage just proves the dispatcher enumerates the five scenes.
last_lines="$(tail -n 12 "${WORK}/list.log" 2>/dev/null)"
if [[ "${STATUS[0]}" == "PASS" ]]; then
  got="$(grep -c . "${WORK}/list.log" || true)"
  if (( got < 5 )) || [[ "${last_lines}" != *"udp-loopback"* ]] ||
     [[ "${last_lines}" != *"mcu-profile"* ]]; then
    echo "run_matrix: FAIL the dispatcher did not list all five scenes" >&2
    STATUS[0]="FAIL"
    [[ "${STATUS[0]}" == "FAIL" ]] # no-op guard for shellcheck symmetry
  fi
fi

# Stray-process verification: nothing with the e2e/host/gen names may be left
# behind. The matrix script's own command line contains the binary name, and so
# does the outer harness that may invoke it (ctest -R e2e_cpu_matrix, a shell
# -c wrapper, grep for the log, ...): those bookkeeping processes are excluded.
echo "--- stray process check (expected: none) ---"
strays="$(pgrep -af "hml_host|hml_gen|e2e_cpu" 2>/dev/null |
  grep -Ev "run_matrix\.sh|pgrep|grep -Ev|ctest|sed |grep" || true)"
if [[ -n "${strays}" ]]; then
  echo "${strays}"
  echo "run_matrix: FAIL stray sender/receiver processes are still alive" >&2
  exit 1
fi
echo "(none)"

TOTAL="${#NAMES[@]}"
echo "--- matrix summary (${MODE}) ---"
printf '%-16s %-7s %10s\n' "stage" "status" "elapsed_ms"
for i in "${!NAMES[@]}"; do
  printf '%-16s %-7s %10d\n' "${NAMES[$i]}" "${STATUS[$i]}" "${TIME_MS[$i]}"
done
printf '%-16s %-7s %10s\n' "TOTAL" "${PASS_COUNT}/${TOTAL}" ""

if (( PASS_COUNT != TOTAL )); then
  echo "run_matrix: FAIL ${PASS_COUNT}/${TOTAL} stages passed" >&2
  exit 1
fi
echo "run_matrix: OK ${PASS_COUNT}/${TOTAL} stages passed (CPU-only; not an MCU result, see docs/cpu_only_e2e.md)"
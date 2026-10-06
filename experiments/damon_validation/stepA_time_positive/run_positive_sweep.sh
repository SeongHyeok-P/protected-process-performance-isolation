#!/usr/bin/env bash
set -euo pipefail
WORKLOAD=${WORKLOAD:-./group_targeted_workload_v2}
OBSERVER=${OBSERVER:-./damon_class_observer_cli_time}
POOL_MIB=${POOL_MIB:-128}
HOT_PAGES_TOTAL=${HOT_PAGES_TOTAL:-3072}
VICTIM_SEC=${VICTIM_SEC:-14}
OBSERVER_SEC=${OBSERVER_SEC:-10}
STAMP=$(date +%Y%m%d_%H%M%S)
ROOTDIR="stepP_sweep_${STAMP}"
mkdir -p "$ROOTDIR"

run_one() {
  local n=$1 groups=$2
  local D="$ROOTDIR/g${n}"
  mkdir -p "$D"
  sudo "$WORKLOAD" --pool-mib "$POOL_MIB" --hot-pages-total "$HOT_PAGES_TOTAL" \
    --groups "$groups" --duration "$VICTIM_SEC" >"$D/workload.out" 2>"$D/workload.err" &
  local WPID=$!
  for _ in $(seq 1 200); do
    if grep -q '^READY ' "$D/workload.err"; then break; fi
    sleep 0.05
  done
  local READY
  READY=$(grep '^READY ' "$D/workload.err" | tail -1 || true)
  [[ -n "$READY" ]] || { echo "g$n READY timeout"; sudo kill -9 "$WPID" || true; return 1; }
  echo "$READY"
  local START END PID
  PID=$(sed -n 's/.*pid=\([0-9]*\).*/\1/p' <<<"$READY")
  START=$(sed -n 's/.*pool_start=\(0x[0-9a-fA-F]*\).*/\1/p' <<<"$READY")
  END=$(sed -n 's/.*pool_end=\(0x[0-9a-fA-F]*\).*/\1/p' <<<"$READY")
  sudo kill -CONT "$PID"
  for _ in $(seq 1 100); do
    if grep -q '^RESUMED ' "$D/workload.err"; then break; fi
    sleep 0.02
  done
  local ORC=0
  sudo "$OBSERVER" --duration "$OBSERVER_SEC" --warmup-ms 1000 --window-ms 1000 \
    --workset-start "$START" --workset-end "$END" --restrict-workset "$PID" \
    >"$D/observer.log" 2>"$D/observer.err" || ORC=$?
  local VRC=0
  wait "$WPID" || VRC=$?
  python3 ./analyze_positive.py "$D/workload.out" "$D/observer.log" | tee "$D/positive_summary.txt"
  python3 ./analyze_time_windows.py "$D/observer.log" > "$D/time_summary.txt"
  echo "victim_rc=$VRC observer_rc=$ORC" | tee "$D/control.txt"
  cat "$D/observer.err"
  local STRUCT
  STRUCT=$(grep '^TIME_STRUCTURE_CHECK=' "$D/time_summary.txt" | tail -1 | cut -d= -f2 || true)
  if [[ "$VRC" -ne 0 || "$ORC" -ne 0 || "$STRUCT" != "PASS" ]]; then
    echo "g$n gate FAIL victim_rc=$VRC observer_rc=$ORC structure=${STRUCT:-missing}" >&2
    return 1
  fi
  if ! grep -q '^PFN_VERIFY_ALL,moved=0,unreadable=0,' "$D/workload.out"; then
    echo "g$n gate FAIL: full-pool PFN stability did not pass" >&2
    return 1
  fi
  if grep -q '\[ERROR\]' "$D/observer.err"; then
    echo "g$n gate FAIL: observer.err contains [ERROR]" >&2
    return 1
  fi
  echo "g$n gate PASS"
}

run_one 4  '0,8,16,24'
run_one 8  '0,4,8,12,16,20,24,28'
run_one 16 '0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30'

echo "RESULT_DIR=$ROOTDIR"

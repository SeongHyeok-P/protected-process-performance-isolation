#!/usr/bin/env bash
set -euo pipefail
VICTIM=${VICTIM:-./perf_mem_probe_diag}
OBSERVER=${OBSERVER:-./damon_class_observer_cli_time}
SIZE_MIB=${SIZE_MIB:-64}
VICTIM_SEC=${VICTIM_SEC:-14}
OBSERVER_SEC=${OBSERVER_SEC:-10}
STAMP=$(date +%Y%m%d_%H%M%S)
DIR="stepA_time_${STAMP}"
mkdir -p "$DIR"

"$VICTIM" "$SIZE_MIB" "$VICTIM_SEC" >"$DIR/victim.out" 2>"$DIR/victim.err" &
VPID=$!
for _ in $(seq 1 100); do
  if grep -q '^READY ' "$DIR/victim.err"; then break; fi
  sleep 0.05
done
READY=$(grep '^READY ' "$DIR/victim.err" | tail -1 || true)
[[ -n "$READY" ]] || { echo "victim READY timeout"; kill -9 "$VPID" || true; exit 1; }
echo "$READY"
START=$(sed -n 's/.*next_start=\(0x[0-9a-fA-F]*\).*/\1/p' <<<"$READY")
END=$(sed -n 's/.*next_end=\(0x[0-9a-fA-F]*\).*/\1/p' <<<"$READY")
kill -CONT "$VPID"
for _ in $(seq 1 100); do
  if grep -q '^RESUMED ' "$DIR/victim.err"; then break; fi
  sleep 0.02
done

echo "workset=$START-$END"
sudo "$OBSERVER" --duration "$OBSERVER_SEC" --warmup-ms 1000 --window-ms 1000 \
  --workset-start "$START" --workset-end "$END" --restrict-workset "$VPID" \
  >"$DIR/observer.log" 2>"$DIR/observer.err" || ORC=$?
ORC=${ORC:-0}
wait "$VPID" || VRC=$?
VRC=${VRC:-0}
python3 ./analyze_time_windows.py "$DIR/observer.log" | tee "$DIR/time_summary.txt"
echo "victim_rc=$VRC observer_rc=$ORC" | tee "$DIR/control.txt"
grep -E 'AGG_SYNC|AGG_COMPLETE|AGG_WARMUP_DROP|WINDOW_TIME_META|TIME_BOUNDARY_SUMMARY|OBSERVER_PARSE_STATS' "$DIR/observer.log" > "$DIR/timing_markers.txt" || true
cat "$DIR/observer.err"
echo "RESULT_DIR=$DIR"

STRUCT=$(grep '^TIME_STRUCTURE_CHECK=' "$DIR/time_summary.txt" | tail -1 | cut -d= -f2 || true)
if [[ "$VRC" -ne 0 || "$ORC" -ne 0 || "$STRUCT" != "PASS" ]]; then
  echo "TIME_NEGATIVE_GATE=FAIL victim_rc=$VRC observer_rc=$ORC structure=${STRUCT:-missing}" >&2
  exit 1
fi
if grep -q '\[ERROR\]' "$DIR/observer.err"; then
  echo "TIME_NEGATIVE_GATE=FAIL observer.err contains [ERROR]" >&2
  exit 1
fi
echo "TIME_NEGATIVE_GATE=PASS"

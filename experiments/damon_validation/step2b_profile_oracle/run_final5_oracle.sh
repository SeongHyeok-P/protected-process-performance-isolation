#!/usr/bin/env bash
set -euo pipefail

DAMON=/sys/kernel/mm/damon/admin
TRACE=/sys/kernel/tracing
POOL_MIB="${POOL_MIB:-160}"
HOT_PAGES="${HOT_PAGES:-16384}"
RUNS="${RUNS:-5}"
MAX_REGIONS="${MAX_REGIONS:-2000}"
MIN_REGIONS="${MIN_REGIONS:-128}"
SAMPLE_US="${SAMPLE_US:-5000}"
AGGR_US="${AGGR_US:-100000}"
UPDATE_US="${UPDATE_US:-1000000}"
WARMUP_SEC="${WARMUP_SEC:-4}"
PROFILE_SEC="${PROFILE_SEC:-8}"
TRACE_SEC="${TRACE_SEC:-15}"
WORKLOAD_DURATION="${WORKLOAD_DURATION:-22}"
MIN_AGGS_PER_WINDOW="${MIN_AGGS_PER_WINDOW:-6}"

VICTIM_CPU="${VICTIM_CPU:-14}"; O0_CPU="${O0_CPU:-2}"; O25_CPU="${O25_CPU:-4}"; O50_CPU="${O50_CPU:-6}"; O75_CPU="${O75_CPU:-8}"; O100_CPU="${O100_CPU:-10}"
VICTIM_GROUPS="24,31,12,23,16,2,10,18,30,21,26,5,11,1,29,3"
COMPLEMENT="0,4,6,7,8,9,13,14,15,17,19,20,22,25,27,28"
O0_GROUPS="$COMPLEMENT"
O25_GROUPS="24,31,12,23,0,4,6,7,8,9,13,14,15,17,19,20"
O50_GROUPS="24,31,12,23,16,2,10,18,0,4,6,7,8,9,13,14"
O75_GROUPS="24,31,12,23,16,2,10,18,30,21,26,5,0,4,6,7"
O100_GROUPS="$VICTIM_GROUPS"

[[ "$POOL_MIB" == 160 ]] || { echo '[ERROR] pool_mib is frozen at 160' >&2; exit 2; }
[[ "$HOT_PAGES" == 16384 ]] || { echo '[ERROR] hot_pages_total is frozen at 16384' >&2; exit 2; }
[[ "$MAX_REGIONS" == 2000 ]] || { echo '[ERROR] max_regions is frozen at 2000 per kdamond' >&2; exit 2; }
[[ "$RUNS" == 5 ]] || { echo '[ERROR] final holdout freezes runs=5' >&2; exit 2; }
sudo -n true 2>/dev/null || { echo '[ERROR] run sudo -v first' >&2; exit 1; }
./build_final5_oracle.sh

STAMP="$(date +%Y%m%d_%H%M%S)"; ROOT="step2b_final5_oracle_${STAMP}"; mkdir -p "$ROOT"
CAL_PATH=/run/protected-daemon/dram-map.json
sudo cat "$CAL_PATH" > "$ROOT/dram-map.frozen.json"
FROZEN_MASK_ID="$(python3 - "$ROOT/dram-map.frozen.json" <<'PY2'
import json,sys
print(json.load(open(sys.argv[1]))['mask_set_id'])
PY2
)"
FROZEN_CAL_SHA="$(sha256sum "$ROOT/dram-map.frozen.json" | awk '{print $1}')"
printf 'FROZEN_CALIBRATION,mask_set_id=%s,sha256=%s\n' "$FROZEN_MASK_ID" "$FROZEN_CAL_SHA"
uname -a > "$ROOT/uname.txt"
sha256sum build_final5_oracle.sh run_final5_oracle.sh analyze_profile_repeatability.py analyze_final5_j_obs.py step2b_group_oracle_workload_profile.c > "$ROOT/source_sha256.txt"
cat > "$ROOT/FROZEN_INPUTS.txt" <<EOF
nr_kdamonds=6
contexts_per_kdamond=1
targets_per_context=1
max_regions_per_kdamond=$MAX_REGIONS
min_regions=$MIN_REGIONS
sample_us=$SAMPLE_US
aggr_us=$AGGR_US
update_us=$UPDATE_US
runs=$RUNS
validation_role=final_holdout
warmup_sec=$WARMUP_SEC
profile_sec=$PROFILE_SEC
window_sec=1
min_aggs_per_window=$MIN_AGGS_PER_WINDOW
hard_gate_per_run=strict_Jobs_monotonic_and_spearman_rho_1
hard_gate_aggregate=min_adjacent_mean_gap_ge_3_pooled_sd
no_semantic_run_replacement=true
pool_mib=$POOL_MIB
hot_pages_total=$HOT_PAGES
estimator=WEIGHTED_NORM_alpha0
EOF

cat > "$ROOT/plan.csv" <<EOF
kdamond_id,name,groups
0,victim,${VICTIM_GROUPS//,/:}
1,o0,${O0_GROUPS//,/:}
2,o25,${O25_GROUPS//,/:}
3,o50,${O50_GROUPS//,/:}
4,o75,${O75_GROUPS//,/:}
5,o100,${O100_GROUPS//,/:}
EOF

names=(victim o0 o25 o50 o75 o100)
roles=(victim aggressor aggressor aggressor aggressor aggressor)
cpus=("$VICTIM_CPU" "$O0_CPU" "$O25_CPU" "$O50_CPU" "$O75_CPU" "$O100_CPU")
groups=("$VICTIM_GROUPS" "$O0_GROUPS" "$O25_GROUPS" "$O50_GROUPS" "$O75_GROUPS" "$O100_GROUPS")
seeds=(2026090201 2026090202 2026090203 2026090204 2026090205 2026090206)

mono_ns(){ python3 - <<'PY'
import time; print(time.monotonic_ns())
PY
}
wait_until_ns(){ local t="$1"; while :; do local n; n="$(mono_ns)"; ((n>=t)) && return; python3 - "$((t-n))" <<'PY'
import sys,time; time.sleep(min(max(int(sys.argv[1]),0)/1e9,0.2))
PY
done; }
wait_pat(){ local f="$1" p="$2" lim="$3" s="$(date +%s)"; while ! grep -q "$p" "$f" 2>/dev/null; do sleep .1; (( $(date +%s)-s < lim )) || { echo "[ERROR] timeout: $p in $f" >&2; tail -40 "$f" >&2||true; return 1; }; done; }
field(){ grep -m1 "^$2" "$1" | sed -n "s/.*$3=\([0-9][0-9]*\).*/\1/p"; }
stop_damon(){ set +e; for k in {0..5}; do sudo sh -c "echo off > '$DAMON/kdamonds/$k/state'" 2>/dev/null||true; done; sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'" 2>/dev/null||true; set -e; }
trap stop_damon EXIT INT TERM

printf 'FINAL5_PLAN,runs=%s,kdamonds=6,max_each=%s,profile_sec=%s,mask_set_id=%s\n' "$RUNS" "$MAX_REGIONS" "$PROFILE_SEC" "$FROZEN_MASK_ID"

for run in $(seq 1 "$RUNS"); do
  dir="$ROOT/run$run"; mkdir -p "$dir"; echo "===== FINAL5_RUN $run/$RUNS ====="
  live_sha="$(sudo sha256sum "$CAL_PATH" | awk '{print $1}')"
  [[ "$live_sha" == "$FROZEN_CAL_SHA" ]] || { echo "[ERROR] calibration changed before run $run" >&2; exit 1; }
  stop_damon
  jobs=(); pids=(); tracer=""
  cleanup_run(){ set +e; [[ -n "$tracer" ]]&&kill -TERM "$tracer" 2>/dev/null||true; ((${#pids[@]}))&&sudo kill -TERM "${pids[@]}" 2>/dev/null||true; for j in "${jobs[@]}"; do kill -TERM "$j" 2>/dev/null||true; done; wait 2>/dev/null||true; stop_damon; set -e; }
  trap 'cleanup_run; exit 130' INT TERM

  echo 'name,pid' > "$dir/pid_map.csv"
  for i in "${!names[@]}"; do
    streams=16; [[ "${roles[$i]}" == victim ]]&&streams=1
    sudo ./step2b_group_oracle_workload_profile \
      --name "${names[$i]}" --role "${roles[$i]}" --pool-mib "$POOL_MIB" \
      --hot-pages-total "$HOT_PAGES" --duration "$WORKLOAD_DURATION" \
      --streams "$streams" --cpu "${cpus[$i]}" --seed "${seeds[$i]}" \
      --groups "${groups[$i]}" --page-map "$dir/${names[$i]}.pages.csv" \
      >"$dir/${names[$i]}.out" 2>"$dir/${names[$i]}.err" & jobs+=("$!")
  done
  for name in "${names[@]}"; do
    wait_pat "$dir/$name.err" '^READY ' 180
    pid="$(field "$dir/$name.err" READY pid)"; [[ -n "$pid" ]]||exit 1
    pids+=("$pid"); echo "$name,$pid" >> "$dir/pid_map.csv"
    [[ -s "$dir/$name.pages.csv" ]] || { echo "[ERROR] missing $name page map" >&2; exit 1; }
  done

  # Ensure all workloads used the same runtime calibration/mask set.
  mask_ids=()
  for name in "${names[@]}"; do
    mid="$(grep -m1 '^READY ' "$dir/$name.err" | sed -n 's/.*mask_set_id=\([^ ]*\).*/\1/p')"
    [[ -n "$mid" ]] || { echo "[ERROR] missing mask_set_id for $name" >&2; exit 1; }
    mask_ids+=("$mid")
  done
  first_mid="${mask_ids[0]}"; for mid in "${mask_ids[@]}"; do [[ "$mid" == "$first_mid" ]] || { echo '[ERROR] mask_set_id mismatch across workloads' >&2; exit 1; }; done
  [[ "$first_mid" == "$FROZEN_MASK_ID" ]] || { echo "[ERROR] run $run mask_set_id differs from frozen calibration" >&2; exit 1; }
  echo "MASK_SET,run=$run,mask_set_id=$first_mid"

  sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'; : > '$TRACE/trace'; echo mono > '$TRACE/trace_clock'"
  sudo sh -c "echo 0 > '$DAMON/kdamonds/nr_kdamonds'; echo 6 > '$DAMON/kdamonds/nr_kdamonds'"
  for k in {0..5}; do
    sudo sh -c "echo 1 > '$DAMON/kdamonds/$k/contexts/nr_contexts'; echo vaddr > '$DAMON/kdamonds/$k/contexts/0/operations'; echo 1 > '$DAMON/kdamonds/$k/contexts/0/targets/nr_targets'; echo '${pids[$k]}' > '$DAMON/kdamonds/$k/contexts/0/targets/0/pid_target'; echo '$SAMPLE_US' > '$DAMON/kdamonds/$k/contexts/0/monitoring_attrs/intervals/sample_us'; echo '$AGGR_US' > '$DAMON/kdamonds/$k/contexts/0/monitoring_attrs/intervals/aggr_us'; echo '$UPDATE_US' > '$DAMON/kdamonds/$k/contexts/0/monitoring_attrs/intervals/update_us'; echo '$MIN_REGIONS' > '$DAMON/kdamonds/$k/contexts/0/monitoring_attrs/nr_regions/min'; echo '$MAX_REGIONS' > '$DAMON/kdamonds/$k/contexts/0/monitoring_attrs/nr_regions/max'"
  done
  sudo sh -c "echo 1 > '$TRACE/events/damon/damon_aggregated/enable'"
  for k in {0..5}; do sudo sh -c "echo on > '$DAMON/kdamonds/$k/state'"; done

  echo 'kdamond_id,kdamond_pid,target_pid,name' > "$dir/kdamond_map.csv"
  kpids=()
  for k in {0..5}; do
    kp=''; for _ in $(seq 1 100); do kp="$(sudo cat "$DAMON/kdamonds/$k/pid" 2>/dev/null||true)"; [[ "$kp" =~ ^[0-9]+$ && "$kp" != 0 ]]&&break; sleep .02; done
    [[ "$kp" =~ ^[0-9]+$ && "$kp" != 0 ]]||{ echo "[ERROR] kdamond $k pid unavailable" >&2; exit 1; }
    kpids+=("$kp"); echo "$k,$kp,${pids[$k]},${names[$k]}" >> "$dir/kdamond_map.csv"
  done
  echo "KDAMOND_MAP,run=$run,$(tail -n +2 "$dir/kdamond_map.csv" | paste -sd';' | sed 's/;/ | /g')"

  sudo timeout "$TRACE_SEC" cat "$TRACE/trace_pipe" > "$dir/raw.trace" & tracer=$!
  sleep .2
  sudo kill -USR1 "${pids[@]}"
  max_resume=0
  for name in "${names[@]}"; do
    wait_pat "$dir/$name.err" '^RESUMED ' 30
    ns="$(field "$dir/$name.err" RESUMED resumed_mono_ns)"; ((ns>max_resume))&&max_resume="$ns"
  done
  measurement_start_ns=$((max_resume + WARMUP_SEC*1000000000))
  measurement_end_ns=$((measurement_start_ns + PROFILE_SEC*1000000000))
  echo "MEASUREMENT_WINDOW,run=$run,start_ns=$measurement_start_ns,end_ns=$measurement_end_ns,seconds=$PROFILE_SEC"
  wait_until_ns "$measurement_end_ns"

  set +e; wait "$tracer"; trc=$?; set -e; tracer=""; [[ $trc -eq 0 || $trc -eq 124 ]] || { echo "[ERROR] tracer rc=$trc" >&2; exit 1; }
  stop_damon
  sudo kill -TERM "${pids[@]}" 2>/dev/null||true
  fail=0; for j in "${jobs[@]}"; do set +e; wait "$j"; rc=$?; set -e; ((rc==0))||fail=1; done; jobs=()
  for name in "${names[@]}"; do grep -q "PFN_VERIFY_ALL,name=${name},moved=0,unreadable=0" "$dir/$name.out" || fail=1; done
  ((fail==0)) || { echo '[ERROR] workload/PFN verify failed' >&2; exit 1; }

  python3 ./analyze_profile_repeatability.py run "$dir/raw.trace" \
    --kdamond-map "$dir/kdamond_map.csv" --page-map-dir "$dir" --plan "$ROOT/plan.csv" \
    --measurement-start-ns "$measurement_start_ns" --profile-sec "$PROFILE_SEC" --run-id "$run" \
    --min-aggs-per-window "$MIN_AGGS_PER_WINDOW" \
    --output-profile-csv "$dir/run_profile.csv" --output-target-csv "$dir/run_targets.csv" \
    --output-window-csv "$dir/run_windows.csv" | tee "$dir/summary.txt"

  trap - INT TERM
  cleanup_run
  sleep 1
done

# Final semantic holdout: do not apply the exploratory cross-run raw-profile equality gate.
# The acceptance criterion is actual 32-bin weighted-Jaccard ordering in all five new runs,
# plus aggregate adjacent separation >= 3 pooled SD.
set +e
python3 ./analyze_final5_j_obs.py "$ROOT" \
  --expected-runs "$RUNS" \
  --min-gap-over-pooled-sd 3.0 \
  --output-csv "$ROOT/final5_j_obs.csv" | tee "$ROOT/final5_oracle.txt"
gate_rc=${PIPESTATUS[0]}
set -e
live_sha="$(sudo sha256sum "$CAL_PATH" | awk '{print $1}')"
[[ "$live_sha" == "$FROZEN_CAL_SHA" ]] || { echo '[ERROR] calibration changed during final holdout' >&2; exit 1; }
if (( gate_rc==0 )); then echo 'STEP2B_FINAL5_ORACLE=PASS'; else echo 'STEP2B_FINAL5_ORACLE=FAIL'; fi
echo "RESULT_DIR=$ROOT"
exit "$gate_rc"

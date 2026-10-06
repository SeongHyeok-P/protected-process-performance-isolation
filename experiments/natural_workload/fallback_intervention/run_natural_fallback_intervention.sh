#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ $EUID -ne 0 ]]; then
    echo "[ERROR] Run with sudo: sudo $0" >&2
    exit 1
fi

RUN_USER="${SUDO_USER:-root}"
USER_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
[[ -n "$USER_HOME" ]] || USER_HOME="/home/$RUN_USER"

REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
V2_ROOT="${V2_ROOT:-$REPO_ROOT/protected-daemon/v2}"
CALIBRATION="${CALIBRATION:-/run/protected-daemon/dram-map.json}"
GRAPH="${GRAPH:-$REPO_ROOT/datasets/kron_g23.sg}"
BFS_BIN="${BFS_BIN:-$REPO_ROOT/benchmark/gapbs/bfs}"
CC_BIN="${CC_BIN:-$REPO_ROOT/benchmark/gapbs/cc}"
PR_BIN="${PR_BIN:-$REPO_ROOT/benchmark/gapbs/pr}"

BLOCKS="${BLOCKS:-5}"
GAPBS_TRIALS="${GAPBS_TRIALS:-1000}"
OMP_THREADS="${OMP_THREADS:-2}"

VICTIM_CPU="${VICTIM_CPU:-14}"
BFS_CPUS="${BFS_CPUS:-2,4}"
CC_CPUS="${CC_CPUS:-6,8}"
PR_CPUS="${PR_CPUS:-10,12}"

VICTIM_MIB="${VICTIM_MIB:-64}"
VICTIM_EPOCH_MS="${VICTIM_EPOCH_MS:-1000}"
LATENCY_EPOCHS="${LATENCY_EPOCHS:-4}"

WORKLOAD_WARMUP_SEC="${WORKLOAD_WARMUP_SEC:-3}"
DAMON_SETTLE_SEC="${DAMON_SETTLE_SEC:-2}"
RELEASE_SETTLE_SEC="${RELEASE_SETTLE_SEC:-2}"
THROTTLE_SETTLE_SEC="${THROTTLE_SETTLE_SEC:-2}"

# Same fixed intervention as the earlier controlled candidate experiment.
# This is CPU time, not a memory-bandwidth percentage.
THROTTLE_CPU_MAX="${THROTTLE_CPU_MAX:-20000 100000}"
UNLIMITED_CPU_MAX="${UNLIMITED_CPU_MAX:-max 100000}"

MEASURE_TRAFFIC="${MEASURE_TRAFFIC:-1}"
TRAFFIC_SECONDS="${TRAFFIC_SECONDS:-1}"
TRAFFIC_EVENT="${TRAFFIC_EVENT:-cpu_core/mem_load_l3_miss_retired.local_dram/pp}"

STAMP="$(date +%Y%m%d_%H%M%S)"
RESULT_DIR="${RESULT_DIR:-$SCRIPT_DIR/results/natural_fallback_$STAMP}"
mkdir -p "$RESULT_DIR/raw"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$RESULT_DIR/run.log"; }
die() { echo "[ERROR] $*" | tee -a "$RESULT_DIR/run.log" >&2; exit 1; }

for p in "$CALIBRATION" "$GRAPH" "$BFS_BIN" "$CC_BIN" "$PR_BIN"; do
    [[ -e "$p" ]] || die "missing: $p"
done

# Refuse a known historical proc_activity bug.
if [[ -f "$V2_ROOT/src/proc_activity.c" ]] &&
   grep -q 'new_sample->utime_ticks[[:space:]]*+[[:space:]]*old_sample->stime_ticks' \
      "$V2_ROOT/src/proc_activity.c"; then
    die "V2_ROOT contains the historical proc_activity stime bug. Use the corrected v2."
fi

if [[ ! -x "$SCRIPT_DIR/natural_victim_probe" || ! -x "$SCRIPT_DIR/natural_score_probe" ]]; then
    log "building helpers against $V2_ROOT"
    sudo -u "$RUN_USER" env HOME="$USER_HOME" V2_ROOT="$V2_ROOT" make -C "$SCRIPT_DIR" all
fi
[[ -x "$SCRIPT_DIR/natural_victim_probe" ]] || die "natural_victim_probe build failed"
[[ -x "$SCRIPT_DIR/natural_score_probe" ]] || die "natural_score_probe build failed"

if [[ "$MEASURE_TRAFFIC" == "1" ]]; then
    if ! command -v perf >/dev/null 2>&1; then
        log "perf not found; traffic diagnostic disabled"
        MEASURE_TRAFFIC=0
    elif ! perf stat -e "$TRAFFIC_EVENT" -- sleep 0.05 >/dev/null 2>"$RESULT_DIR/raw/perf_preflight.err"; then
        log "traffic event unavailable; traffic diagnostic disabled: $TRAFFIC_EVENT"
        MEASURE_TRAFFIC=0
    else
        log "traffic diagnostic enabled: $TRAFFIC_EVENT"
    fi
fi

{
    echo "timestamp=$STAMP"
    echo "run_user=$RUN_USER"
    echo "V2_ROOT=$V2_ROOT"
    echo "calibration=$CALIBRATION"
    echo "graph=$GRAPH"
    echo "BFS_BIN=$BFS_BIN"
    echo "CC_BIN=$CC_BIN"
    echo "PR_BIN=$PR_BIN"
    echo "blocks=$BLOCKS"
    echo "gapbs_trials=$GAPBS_TRIALS"
    echo "omp_threads=$OMP_THREADS"
    echo "victim_cpu=$VICTIM_CPU"
    echo "bfs_cpus=$BFS_CPUS"
    echo "cc_cpus=$CC_CPUS"
    echo "pr_cpus=$PR_CPUS"
    echo "victim_mib=$VICTIM_MIB"
    echo "victim_epoch_ms=$VICTIM_EPOCH_MS"
    echo "latency_epochs=$LATENCY_EPOCHS"
    echo "throttle_cpu_max=$THROTTLE_CPU_MAX"
    echo "traffic_enabled=$MEASURE_TRAFFIC"
    echo "traffic_event=$TRAFFIC_EVENT"
    echo
    uname -a
    echo
    lscpu || true
    echo
    sha256sum "$CALIBRATION" || true
    echo
    git -C "$V2_ROOT" rev-parse HEAD 2>/dev/null || true
    git -C "$V2_ROOT" status --short 2>/dev/null || true
} > "$RESULT_DIR/metadata.txt"
cp "$CALIBRATION" "$RESULT_DIR/dram-map.json"

echo "block,base_ns" > "$RESULT_DIR/blocks.csv"
echo "block,candidate,pid,activity,cpu_score,fault_score,rss_score,legacy_total,cpu_ratio,faults_per_sec,rss_mib,active,overlap,fused" > "$RESULT_DIR/scores.csv"
echo "block,order_pos,condition,target,before_ns,after_ns,raw_recovery_ns,usage_usec_delta,nr_throttled_delta,throttled_usec_delta,traffic_before,traffic_after,traffic_removed" > "$RESULT_DIR/interventions.csv"

CGROOT="/sys/fs/cgroup/natural_fallback_$$"
BFS_PID=""; CC_PID=""; PR_PID=""; VICTIM_PID=""

cleanup_block_processes() {
    set +e
    for p in "$BFS_PID" "$CC_PID" "$PR_PID" "$VICTIM_PID"; do
        [[ -n "$p" ]] && kill "$p" 2>/dev/null
    done
    sleep 0.2
    for p in "$BFS_PID" "$CC_PID" "$PR_PID" "$VICTIM_PID"; do
        [[ -n "$p" ]] && kill -9 "$p" 2>/dev/null
        [[ -n "$p" ]] && wait "$p" 2>/dev/null
    done
    BFS_PID=""; CC_PID=""; PR_PID=""; VICTIM_PID=""
    set -e
}

cleanup_all() {
    set +e
    cleanup_block_processes
    if [[ -d "$CGROOT" ]]; then
        for g in bfs cc pr; do
            [[ -d "$CGROOT/$g" ]] && rmdir "$CGROOT/$g" 2>/dev/null
        done
        rmdir "$CGROOT" 2>/dev/null
    fi
    set -e
}
trap cleanup_all EXIT INT TERM

[[ -f /sys/fs/cgroup/cgroup.controllers ]] || die "cgroup v2 unified hierarchy not found"
grep -qw cpu /sys/fs/cgroup/cgroup.controllers || die "cpu controller unavailable in cgroup v2"
if ! grep -qw cpu /sys/fs/cgroup/cgroup.subtree_control; then
    echo +cpu > /sys/fs/cgroup/cgroup.subtree_control || die "cannot enable root cpu controller"
fi
mkdir "$CGROOT"
echo +cpu > "$CGROOT/cgroup.subtree_control"
for g in bfs cc pr; do
    mkdir "$CGROOT/$g"
    echo "$UNLIMITED_CPU_MAX" > "$CGROOT/$g/cpu.max"
done

set_unlimited_all() {
    for g in bfs cc pr; do echo "$UNLIMITED_CPU_MAX" > "$CGROOT/$g/cpu.max"; done
}

assert_alive() {
    local name="$1" pid="$2"
    kill -0 "$pid" 2>/dev/null || die "$name pid=$pid exited early; inspect raw logs"
}

wait_victim_ready() {
    local logf="$1"
    for _ in $(seq 1 600); do
        grep -q '^READY,' "$logf" 2>/dev/null && return 0
        [[ -n "$VICTIM_PID" ]] && kill -0 "$VICTIM_PID" 2>/dev/null || die "victim exited before READY: $logf"
        sleep 0.1
    done
    die "victim READY timeout: $logf"
}

start_victim() {
    local block="$1" vlog="$RESULT_DIR/raw/block_${1}_victim.log" seed=$((2026100100 + block))
    taskset -c "$VICTIM_CPU" "$SCRIPT_DIR/natural_victim_probe" \
        "$VICTIM_MIB" "$VICTIM_EPOCH_MS" "$seed" \
        >"$vlog" 2>"$RESULT_DIR/raw/block_${block}_victim.err" &
    VICTIM_PID=$!
    wait_victim_ready "$vlog"
    log "block=$block victim READY pid=$VICTIM_PID"
}

start_one_gapbs() {
    local block="$1" label="$2" cpus="$3" bin="$4" cgroup="$5"
    local outf="$RESULT_DIR/raw/block_${block}_${label}.log"
    env OMP_NUM_THREADS="$OMP_THREADS" OMP_PROC_BIND=true \
        taskset -c "$cpus" "$bin" -f "$GRAPH" -n "$GAPBS_TRIALS" \
        >"$outf" 2>&1 &
    local pid=$!
    echo "$pid" > "$CGROOT/$cgroup/cgroup.procs"
    sleep 0.1
    kill -0 "$pid" 2>/dev/null || die "$label exited at launch; inspect $outf (local GAPBS CLI may differ)"
    case "$label" in bfs) BFS_PID="$pid" ;; cc) CC_PID="$pid" ;; pagerank) PR_PID="$pid" ;; esac
    log "block=$block $label pid=$pid cpus=$cpus"
}

start_candidates() {
    local block="$1"
    start_one_gapbs "$block" bfs "$BFS_CPUS" "$BFS_BIN" bfs
    start_one_gapbs "$block" cc "$CC_CPUS" "$CC_BIN" cc
    start_one_gapbs "$block" pagerank "$PR_CPUS" "$PR_BIN" pr
    sleep "$WORKLOAD_WARMUP_SEC"
    assert_alive bfs "$BFS_PID"; assert_alive cc "$CC_PID"; assert_alive pagerank "$PR_PID"
}

measure_latency() {
    local block="$1" vlog="$RESULT_DIR/raw/block_${1}_victim.log"
    local start_count epoch_s
    start_count="$(grep -c '^VICTIM_SAMPLE,' "$vlog" 2>/dev/null || true)"
    epoch_s="$(awk -v ms="$VICTIM_EPOCH_MS" -v n="$LATENCY_EPOCHS" 'BEGIN{printf "%.3f",((n+2)*ms)/1000.0}')"
    sleep "$epoch_s"
    awk -F, -v start="$start_count" -v need="$LATENCY_EPOCHS" '
        /^VICTIM_SAMPLE,/ {
            c++
            if (c > start+1 && c <= start+1+need) {split($5,a,"="); sum+=a[2]; n++}
        }
        END {if(n!=need) exit 2; printf "%.9f\n",sum/n}
    ' "$vlog" || die "not enough complete victim epochs: $vlog"
}

cpu_stat_value() {
    local group="$1" key="$2"
    awk -v k="$key" '$1==k{print $2;found=1} END{if(!found)print 0}' "$CGROOT/$group/cpu.stat"
}

target_pid() { case "$1" in bfs) echo "$BFS_PID";; cc) echo "$CC_PID";; pagerank) echo "$PR_PID";; *) echo "";; esac; }
target_group() { case "$1" in bfs) echo bfs;; cc) echo cc;; pagerank) echo pr;; *) echo "";; esac; }

tid_list() {
    local pid="$1" first=1 out="" d tid
    for d in /proc/"$pid"/task/*; do
        [[ -d "$d" ]] || continue; tid="${d##*/}"
        if (( first )); then out="$tid"; first=0; else out="$out,$tid"; fi
    done
    echo "$out"
}

measure_traffic() {
    local block="$1" phase="$2" target="$3" pid="$4"
    [[ "$MEASURE_TRAFFIC" == "1" ]] || { echo NA; return 0; }
    local tids out err val
    tids="$(tid_list "$pid")"; [[ -n "$tids" ]] || { echo NA; return 0; }
    out="$RESULT_DIR/raw/block_${block}_${phase}_${target}_perf.csv"
    err="$RESULT_DIR/raw/block_${block}_${phase}_${target}_perf.err"
    if ! perf stat -x, -e "$TRAFFIC_EVENT" -p "$tids" -- sleep "$TRAFFIC_SECONDS" 2>"$out" >"$err"; then
        echo NA; return 0
    fi
    val="$(awk -F, '$1 !~ /<not/ {x=$1;gsub(/[[:space:]]/,"",x); if(x~/^[0-9]+([.][0-9]+)?$/){print x;exit}}' "$out")"
    [[ -n "$val" ]] && echo "$val" || echo NA
}

numeric_delta_or_na() {
    [[ "$1" == NA || "$2" == NA ]] && { echo NA; return; }
    awk -v x="$1" -v y="$2" 'BEGIN{printf "%.9f",x-y}'
}

append_scores() {
    local block="$1" sf="$2"
    awk -F, -v b="$block" '/^SCORE,/ {printf "%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",b,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14}' \
        "$sf" >> "$RESULT_DIR/scores.csv"
}

run_score_probe() {
    local block="$1" sf="$RESULT_DIR/raw/block_${1}_scores.log"
    log "block=$block DAMON score probe ON (6s warmup + 1s measured window)"
    if ! "$SCRIPT_DIR/natural_score_probe" --calibration "$CALIBRATION" \
        "$VICTIM_PID" "$BFS_PID" "$CC_PID" "$PR_PID" \
        >"$sf" 2>"$RESULT_DIR/raw/block_${block}_scores.err"; then
        die "score probe failed block=$block; inspect raw/block_${block}_scores.err"
    fi
    local n; n="$(grep -c '^SCORE,' "$sf" || true)"
    [[ "$n" == 3 ]] || die "score probe returned $n/3 candidates in block $block"
    append_scores "$block" "$sf"
    log "block=$block DAMON OFF; settle before clean latency measurements"
    sleep "$DAMON_SETTLE_SEC"
}

condition_order() {
    case "$1" in
        1) echo "UNCTRL T_BFS T_CC T_PR";;
        2) echo "T_BFS T_CC T_PR UNCTRL";;
        3) echo "T_CC T_PR UNCTRL T_BFS";;
        4) echo "T_PR UNCTRL T_BFS T_CC";;
        5) echo "UNCTRL T_PR T_CC T_BFS";;
        *) local r=$((($1-1)%4)); local arr=(UNCTRL T_BFS T_CC T_PR) out=() i; for i in 0 1 2 3; do out+=("${arr[$(((i+r)%4))]}"); done; echo "${out[*]}";;
    esac
}

run_condition() {
    local block="$1" order_pos="$2" cond="$3" target=none
    case "$cond" in T_BFS) target=bfs;; T_CC) target=cc;; T_PR) target=pagerank;; UNCTRL) target=none;; *) die "unknown condition $cond";; esac

    assert_alive bfs "$BFS_PID"; assert_alive cc "$CC_PID"; assert_alive pagerank "$PR_PID"; assert_alive victim "$VICTIM_PID"
    set_unlimited_all; sleep "$RELEASE_SETTLE_SEC"

    local before after raw group="" pid=""
    local usage0=0 nr0=0 thr0=0 usage1=0 nr1=0 thr1=0
    local traffic_before=NA traffic_after=NA traffic_removed=NA
    before="$(measure_latency "$block")"

    if [[ "$target" != none ]]; then
        group="$(target_group "$target")"; pid="$(target_pid "$target")"
        usage0="$(cpu_stat_value "$group" usage_usec)"; nr0="$(cpu_stat_value "$group" nr_throttled)"; thr0="$(cpu_stat_value "$group" throttled_usec)"
        traffic_before="$(measure_traffic "$block" pre "$target" "$pid")"
        echo "$THROTTLE_CPU_MAX" > "$CGROOT/$group/cpu.max"
        sleep "$THROTTLE_SETTLE_SEC"
        after="$(measure_latency "$block")"
        traffic_after="$(measure_traffic "$block" post "$target" "$pid")"
        usage1="$(cpu_stat_value "$group" usage_usec)"; nr1="$(cpu_stat_value "$group" nr_throttled)"; thr1="$(cpu_stat_value "$group" throttled_usec)"
        traffic_removed="$(numeric_delta_or_na "$traffic_before" "$traffic_after")"
    else
        sleep "$THROTTLE_SETTLE_SEC"
        after="$(measure_latency "$block")"
    fi

    raw="$(awk -v a="$before" -v b="$after" 'BEGIN{printf "%.9f",a-b}')"
    local du=$((usage1-usage0)) dn=$((nr1-nr0)) dt=$((thr1-thr0))
    echo "$block,$order_pos,$cond,$target,$before,$after,$raw,$du,$dn,$dt,$traffic_before,$traffic_after,$traffic_removed" >> "$RESULT_DIR/interventions.csv"
    log "block=$block pos=$order_pos $cond before=$before after=$after raw_recovery=$raw ns"
    set_unlimited_all
}

log "results: $RESULT_DIR"
log "configuration: blocks=$BLOCKS victim=$VICTIM_MIB MiB quota='$THROTTLE_CPU_MAX'"

for block in $(seq 1 "$BLOCKS"); do
    log "================ BLOCK $block/$BLOCKS ================"
    cleanup_block_processes; set_unlimited_all
    start_victim "$block"
    sleep 1
    base_ns="$(measure_latency "$block")"
    echo "$block,$base_ns" >> "$RESULT_DIR/blocks.csv"
    log "block=$block BASE victim-only=$base_ns ns/read"

    start_candidates "$block"
    run_score_probe "$block"

    order="$(condition_order "$block")"
    log "block=$block intervention order: $order"
    pos=0
    for cond in $order; do pos=$((pos+1)); run_condition "$block" "$pos" "$cond"; done
    cleanup_block_processes
done

log "analyzing"
sudo -u "$RUN_USER" env HOME="$USER_HOME" python3 "$SCRIPT_DIR/analyze_natural_fallback.py" "$RESULT_DIR" | tee "$RESULT_DIR/analysis.console.txt"
log "DONE"
log "summary: $RESULT_DIR/summary.txt"

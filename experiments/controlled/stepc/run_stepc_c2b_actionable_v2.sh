#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Step C2-B: multi-candidate actionable recovery
#
# Purpose:
#   With 5 aggressors running together, throttle ONE candidate
#   and measure how much victim latency recovers.
#
# This measures policy/actionable ground truth, not total harm.
#
# Scenario MIX_A:
#   o0=HIGH, o25=LOW, o50=HIGH, o75=LOW, o100=HIGH
#
# Scenario MIX_B:
#   o0=LOW,  o25=HIGH,o50=LOW,  o75=HIGH,o100=LOW
#
# Run one scenario at a time:
#   SCENARIO=A ./run_stepc_c2b_actionable.sh
#   SCENARIO=B ./run_stepc_c2b_actionable.sh
#
# Throttle:
#   selected candidate cpu.max = 20000 100000 (20%)
#   others = max 100000
#
# Why 20%:
#   LOW pacing is already ~30% of HIGH throughput; 50% quota may
#   not bind LOW candidates. 20% gives the same concrete cgroup
#   action to every candidate.
#
# DAMON OFF throughout.
# ============================================================

ROOT=/sys/kernel/mm/damon/admin
TRACE=/sys/kernel/tracing
CGROOT="${CGROOT:-/sys/fs/cgroup}"
W="${W:-./stepc_activity_overlap_workload}"

SCENARIO="${SCENARIO:-A}"
OUTDIR="${OUTDIR:-stepc_c2b_actionable_${SCENARIO}}"

POOL_MIB="${POOL_MIB:-160}"
HOT_PAGES="${HOT_PAGES:-16384}"
WORKLOAD_DURATION="${WORKLOAD_DURATION:-60}"
MEASURE_SEC="${MEASURE_SEC:-20}"

LOW_PACE_US="${LOW_PACE_US:-200}"
THROTTLE_QUOTA="${THROTTLE_QUOTA:-20000}"
THROTTLE_PERIOD="${THROTTLE_PERIOD:-100000}"

VICTIM_CPU=14
NAMES=(o0 o25 o50 o75 o100)
CPUS=(2 4 6 8 10)
OVERLAPS=(0 25 50 75 100)

VICTIM_GROUPS="24,31,12,23,16,2,10,18,30,21,26,5,11,1,29,3"
GROUPS_o0="0,4,6,7,8,9,13,14,15,17,19,20,22,25,27,28"
GROUPS_o25="24,31,12,23,0,4,6,7,8,9,13,14,15,17,19,20"
GROUPS_o50="24,31,12,23,16,2,10,18,0,4,6,7,8,9,13,14"
GROUPS_o75="24,31,12,23,16,2,10,18,30,21,26,5,0,4,6,7"
GROUPS_o100="$VICTIM_GROUPS"

# Three balanced-ish blocks.  UNCTRL is included in each block.
BLOCK1=(UNCTRL T_o0 T_o25 T_o50 T_o75 T_o100)
BLOCK2=(T_o50 T_o75 T_o100 UNCTRL T_o0 T_o25)
BLOCK3=(T_o100 UNCTRL T_o0 T_o25 T_o50 T_o75)

if ! sudo -n true 2>/dev/null; then
    echo "[ERROR] run sudo -v first" >&2
    exit 1
fi
[[ -x "$W" ]] || { echo "[ERROR] missing $W" >&2; exit 1; }
grep -qw cpu "$CGROOT/cgroup.controllers" || {
    echo "[ERROR] cgroup v2 cpu controller unavailable under $CGROOT" >&2
    exit 1
}

# Child cgroups get cpu.max only when the parent exposes the cpu controller.
if ! grep -qw cpu "$CGROOT/cgroup.subtree_control"; then
    echo +cpu | sudo tee "$CGROOT/cgroup.subtree_control" >/dev/null || {
        echo "[ERROR] failed to enable +cpu in $CGROOT/cgroup.subtree_control" >&2
        exit 1
    }
fi

case "$SCENARIO" in
    A) ACTIVITIES=(H L H L H) ;;
    B) ACTIVITIES=(L H L H L) ;;
    *) echo "[ERROR] SCENARIO must be A or B" >&2; exit 1 ;;
esac

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"
echo "block,position,condition,scenario,victim_ns,o0_mops,o25_mops,o50_mops,o75_mops,o100_mops,throttled" \
    > "$OUTDIR/results.csv"

ACTIVE_PIDS=()
RUNCG=""

cleanup()
{
    set +e
    sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'" 2>/dev/null || true
    for s in "$ROOT"/kdamonds/*/state; do
        sudo test -e "$s" && echo off | sudo tee "$s" >/dev/null 2>&1 || true
    done
    echo 0 | sudo tee "$ROOT/kdamonds/nr_kdamonds" >/dev/null 2>&1 || true

    if ((${#ACTIVE_PIDS[@]})); then
        sudo kill -TERM "${ACTIVE_PIDS[@]}" 2>/dev/null || true
    fi

    if [[ -n "$RUNCG" && -d "$RUNCG" ]]; then
        for d in victim o0 o25 o50 o75 o100; do
            sudo rmdir "$RUNCG/$d" 2>/dev/null || true
        done
        sudo rmdir "$RUNCG" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

hard_disable_damon()
{
    sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'"
    for s in "$ROOT"/kdamonds/*/state; do
        sudo test -e "$s" && echo off | sudo tee "$s" >/dev/null 2>&1 || true
    done
    echo 0 | sudo tee "$ROOT/kdamonds/nr_kdamonds" >/dev/null
}

wait_ready()
{
    local f="$1"
    for _ in $(seq 1 1800); do
        grep -q '^READY ' "$f" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "[ERROR] READY timeout: $f" >&2
    tail -60 "$f" >&2 || true
    exit 1
}

extract_pid()
{
    sed -n 's/^READY pid=\([0-9]*\).*/\1/p' "$1" | head -1
}

groups_for()
{
    case "$1" in
        o0) echo "$GROUPS_o0" ;;
        o25) echo "$GROUPS_o25" ;;
        o50) echo "$GROUPS_o50" ;;
        o75) echo "$GROUPS_o75" ;;
        o100) echo "$GROUPS_o100" ;;
    esac
}

ensure_cpu_controller()
{
    echo "CGROOT_CONTROLLERS=$(cat "$CGROOT/cgroup.controllers")"
    echo "CGROOT_SUBTREE_BEFORE=$(cat "$CGROOT/cgroup.subtree_control")"

    if ! grep -qw cpu "$CGROOT/cgroup.controllers"; then
        echo "[ERROR] cpu controller is not available at $CGROOT" >&2
        exit 1
    fi

    # Children of CGROOT only receive cpu controller files if cpu is enabled
    # in the parent's subtree_control.
    if ! grep -qw cpu "$CGROOT/cgroup.subtree_control"; then
        echo "+cpu" | sudo tee "$CGROOT/cgroup.subtree_control" >/dev/null
    fi

    if ! grep -qw cpu "$CGROOT/cgroup.subtree_control"; then
        echo "[ERROR] failed to enable +cpu in $CGROOT/cgroup.subtree_control" >&2
        exit 1
    fi

    echo "CGROOT_SUBTREE_AFTER=$(cat "$CGROOT/cgroup.subtree_control")"
}

make_cgroups()
{
    RUNCG="$CGROOT/stepc_c2b_${$}_${RANDOM}"

    ensure_cpu_controller

    sudo mkdir "$RUNCG"

    # RUNCG itself must delegate cpu to its children BEFORE child cgroups
    # are used for cpu.max control.
    if ! grep -qw cpu "$RUNCG/cgroup.controllers"; then
        echo "[ERROR] cpu controller did not propagate to $RUNCG" >&2
        echo "RUNCG_CONTROLLERS=$(cat "$RUNCG/cgroup.controllers")" >&2
        exit 1
    fi

    echo "+cpu" | sudo tee "$RUNCG/cgroup.subtree_control" >/dev/null

    if ! grep -qw cpu "$RUNCG/cgroup.subtree_control"; then
        echo "[ERROR] failed to enable +cpu in $RUNCG/cgroup.subtree_control" >&2
        exit 1
    fi

    echo "RUNCG=$RUNCG"
    echo "RUNCG_CONTROLLERS=$(cat "$RUNCG/cgroup.controllers")"
    echo "RUNCG_SUBTREE=$(cat "$RUNCG/cgroup.subtree_control")"

    for n in victim o0 o25 o50 o75 o100; do
        sudo mkdir "$RUNCG/$n"

        if [[ ! -e "$RUNCG/$n/cpu.max" ]]; then
            echo "[ERROR] missing $RUNCG/$n/cpu.max" >&2
            echo "child controllers: $(cat "$RUNCG/$n/cgroup.controllers" 2>/dev/null || true)" >&2
            exit 1
        fi

        echo "max $THROTTLE_PERIOD" |
            sudo tee "$RUNCG/$n/cpu.max" >/dev/null

        echo "CGROUP_READY,name=$n,cpu.max=$(cat "$RUNCG/$n/cpu.max")"
    done
}

remove_cgroups()
{
    set +e
    for d in victim o0 o25 o50 o75 o100; do
        sudo rmdir "$RUNCG/$d" 2>/dev/null || true
    done
    sudo rmdir "$RUNCG" 2>/dev/null || true
    set -e
    RUNCG=""
}

run_one()
{
    local block="$1"
    local pos="$2"
    local cond="$3"
    local rundir="$OUTDIR/block${block}_pos${pos}_${cond}"
    mkdir -p "$rundir"

    hard_disable_damon
    make_cgroups

    echo
    echo "============================================================"
    echo "C2-B scenario=$SCENARIO block=$block pos=$pos cond=$cond"
    echo "============================================================"

    local launchers=()

    sudo "$W" \
        --name victim --role victim --groups "$VICTIM_GROUPS" \
        --pool-mib "$POOL_MIB" --hot-pages-total "$HOT_PAGES" \
        --duration "$WORKLOAD_DURATION" --streams 1 --pace-us 0 \
        --cpu "$VICTIM_CPU" --seed 2026090201 \
        >"$rundir/victim.out" 2>"$rundir/victim.err" &
    launchers+=("$!")

    for i in 0 1 2 3 4; do
        local n="${NAMES[$i]}"
        local a="${ACTIVITIES[$i]}"
        local pace=0
        [[ "$a" == "L" ]] && pace="$LOW_PACE_US"
        local groups
        groups=$(groups_for "$n")

        sudo "$W" \
            --name "$n" --role aggressor --groups "$groups" \
            --pool-mib "$POOL_MIB" --hot-pages-total "$HOT_PAGES" \
            --duration "$WORKLOAD_DURATION" --streams 16 --pace-us "$pace" \
            --cpu "${CPUS[$i]}" --seed "$((2026091300 + OVERLAPS[$i]))" \
            >"$rundir/${n}.out" 2>"$rundir/${n}.err" &
        launchers+=("$!")
    done

    wait_ready "$rundir/victim.err"
    local vpid
    vpid=$(extract_pid "$rundir/victim.err")

    local pids=("$vpid")
    for n in "${NAMES[@]}"; do
        wait_ready "$rundir/${n}.err"
        pids+=("$(extract_pid "$rundir/${n}.err")")
    done
    ACTIVE_PIDS=("${pids[@]}")

    for p in "${pids[@]}"; do
        sudo kill -0 "$p" 2>/dev/null || { echo "[ERROR] dead pid=$p" >&2; exit 1; }
    done

    # Place each workload into its own cgroup before starting active phase.
    echo "$vpid" | sudo tee "$RUNCG/victim/cgroup.procs" >/dev/null
    for i in 0 1 2 3 4; do
        echo "${pids[$((i+1))]}" | sudo tee "$RUNCG/${NAMES[$i]}/cgroup.procs" >/dev/null
    done

    local throttled="NONE"
    if [[ "$cond" != "UNCTRL" ]]; then
        throttled="${cond#T_}"
        echo "$THROTTLE_QUOTA $THROTTLE_PERIOD" |
            sudo tee "$RUNCG/$throttled/cpu.max" >/dev/null
        echo "THROTTLE,target=$throttled,cpu.max=$(sudo cat "$RUNCG/$throttled/cpu.max")"
    fi

    sudo kill -USR1 "${pids[@]}"
    sleep "$MEASURE_SEC"
    sudo kill -TERM "${pids[@]}" 2>/dev/null || true

    set +e
    for j in "${launchers[@]}"; do wait "$j"; done
    set -e
    ACTIVE_PIDS=()

    local vmops
    vmops=$(sed -n 's/.*Mops_per_sec=\([0-9.]*\).*/\1/p' "$rundir/victim.err" | tail -1)
    [[ -n "$vmops" ]] || { echo "[ERROR] missing victim mops" >&2; exit 1; }
    local vns
    vns=$(awk -v m="$vmops" 'BEGIN {printf "%.6f",1000.0/m}')

    local mops=()
    for n in "${NAMES[@]}"; do
        local m
        m=$(sed -n 's/.*Mops_per_sec=\([0-9.]*\).*/\1/p' "$rundir/${n}.err" | tail -1)
        [[ -n "$m" ]] || { echo "[ERROR] missing $n mops" >&2; exit 1; }
        mops+=("$m")
    done

    for n in victim "${NAMES[@]}"; do
        grep -q "PFN_VERIFY_ALL,name=${n},moved=0,unreadable=0" "$rundir/${n}.out" || {
            echo "[ERROR] PFN verification failed: $n" >&2
            exit 1
        }
    done

    echo "RESULT,scenario=$SCENARIO,block=$block,pos=$pos,cond=$cond,victim_ns=$vns,throttled=$throttled"
    echo "$block,$pos,$cond,$SCENARIO,$vns,${mops[0]},${mops[1]},${mops[2]},${mops[3]},${mops[4]},$throttled" \
        >> "$OUTDIR/results.csv"

    remove_cgroups
    sleep 2
}

for block in 1 2 3; do
    arr="BLOCK${block}[@]"
    order=("${!arr}")
    pos=0
    echo "################ BLOCK $block: ${order[*]} ################"
    for cond in "${order[@]}"; do
        pos=$((pos+1))
        run_one "$block" "$pos" "$cond"
    done
done

python3 - "$OUTDIR/results.csv" <<'PY'
import csv, statistics, sys
from collections import defaultdict

path=sys.argv[1]
rows=[]
with open(path,newline="") as f:
    for r in csv.DictReader(f):
        r["block"]=int(r["block"])
        r["victim_ns"]=float(r["victim_ns"])
        for n in ("o0","o25","o50","o75","o100"):
            r[n+"_mops"]=float(r[n+"_mops"])
        rows.append(r)

by_block=defaultdict(dict)
for r in rows:
    by_block[r["block"]][r["condition"]]=r

names=("o0","o25","o50","o75","o100")
recoveries=defaultdict(list)
throughput_change=defaultdict(list)

for b,d in by_block.items():
    base=d["UNCTRL"]["victim_ns"]
    for n in names:
        tr=d["T_"+n]
        rec=base-tr["victim_ns"]
        recoveries[n].append(rec)
        u=d["UNCTRL"][n+"_mops"]
        t=tr[n+"_mops"]
        throughput_change[n].append((t/u) if u else float("nan"))

def ms(xs):
    return statistics.mean(xs), statistics.stdev(xs) if len(xs)>1 else 0.0

print()
print("============================================================")
print("STEPC_C2B_ACTIONABLE_SUMMARY")
print("============================================================")
for n in names:
    rm,rs=ms(recoveries[n])
    tm,ts=ms(throughput_change[n])
    print(f"{n}_RECOVERY_NS_MEAN={rm:.6f}")
    print(f"{n}_RECOVERY_NS_SD={rs:.6f}")
    print(f"{n}_THROTTLED_OVER_UNCTRL_MOPS_MEAN={tm:.6f}")
    print(f"{n}_THROTTLED_OVER_UNCTRL_MOPS_SD={ts:.6f}")

means={n:statistics.mean(recoveries[n]) for n in names}
ranking=sorted(names,key=lambda n:means[n],reverse=True)
print("ACTIONABLE_RECOVERY_RANKING="+">".join(ranking))
print("============================================================")
PY

echo "RESULT_CSV=$OUTDIR/results.csv"
echo "STEPC_C2B_COMPLETE"
trap - EXIT INT TERM

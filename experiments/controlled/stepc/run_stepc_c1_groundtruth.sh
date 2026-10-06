#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# Step C1: DAMON-free ground truth
#
# Conditions:
#   BASE  : victim only
#   L0    : low activity  + 0% overlap
#   H0    : high activity + 0% overlap
#   L100  : low activity  + 100% overlap
#   H100  : high activity + 100% overlap
#
# Design:
#   - DAMON OFF, damon_aggregated tracepoint OFF
#   - victim CPU 14
#   - aggressor CPU 2
#   - Low activity fixed from C0: pace_us=200
#   - High activity: pace_us=0
#   - 5 blocks x 5 conditions = 25 runs
#   - Latin-square style order so each condition occupies each
#     ordinal position once across the 5 blocks
#   - fresh processes / fresh physical allocations every run
#   - same seeds per role so logical access order is held fixed
#
# Ground truth:
#   victim harm = slowdown vs BASE from the same block
# ============================================================

ROOT=/sys/kernel/mm/damon/admin
TRACE=/sys/kernel/tracing

W="${W:-./stepc_activity_overlap_workload}"
OUTDIR="${OUTDIR:-stepc_c1_groundtruth}"

POOL_MIB="${POOL_MIB:-160}"
HOT_PAGES="${HOT_PAGES:-16384}"
WORKLOAD_DURATION="${WORKLOAD_DURATION:-60}"
MEASURE_SEC="${MEASURE_SEC:-20}"

VICTIM_CPU="${VICTIM_CPU:-14}"
AGGR_CPU="${AGGR_CPU:-2}"

LOW_PACE_US="${LOW_PACE_US:-200}"
HIGH_PACE_US=0

VICTIM_GROUPS="24,31,12,23,16,2,10,18,30,21,26,5,11,1,29,3"
O0_GROUPS="0,4,6,7,8,9,13,14,15,17,19,20,22,25,27,28"
O100_GROUPS="$VICTIM_GROUPS"

# Five cyclic orders: each condition appears once in each position.
BLOCK1=(BASE L0 H0 L100 H100)
BLOCK2=(L0 H0 L100 H100 BASE)
BLOCK3=(H0 L100 H100 BASE L0)
BLOCK4=(L100 H100 BASE L0 H0)
BLOCK5=(H100 BASE L0 H0 L100)

if ! sudo -n true 2>/dev/null; then
    echo "[ERROR] sudo credential not active. Run: sudo -v" >&2
    exit 1
fi

if [[ ! -x "$W" ]]; then
    echo "[ERROR] workload binary not executable: $W" >&2
    echo "Run ./build_stepc_c0.sh first." >&2
    exit 1
fi

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

echo "block,position,condition,victim_pid,aggr_pid,victim_mops,victim_ns_per_read,aggr_mops,pace_us,victim_pfn_ok,aggr_pfn_ok" \
    > "$OUTDIR/results.csv"

ACTIVE_VPID=""
ACTIVE_APID=""

cleanup()
{
    set +e

    sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'" \
        2>/dev/null || true

    for s in "$ROOT"/kdamonds/*/state; do
        if sudo test -e "$s"; then
            echo off | sudo tee "$s" >/dev/null 2>&1 || true
        fi
    done
    echo 0 | sudo tee "$ROOT/kdamonds/nr_kdamonds" >/dev/null 2>&1 || true

    [[ -n "${ACTIVE_VPID:-}" ]] && sudo kill -TERM "$ACTIVE_VPID" 2>/dev/null || true
    [[ -n "${ACTIVE_APID:-}" ]] && sudo kill -TERM "$ACTIVE_APID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

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

hard_disable_damon()
{
    sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'"

    for s in "$ROOT"/kdamonds/*/state; do
        if sudo test -e "$s"; then
            echo off | sudo tee "$s" >/dev/null 2>&1 || true
        fi
    done
    echo 0 | sudo tee "$ROOT/kdamonds/nr_kdamonds" >/dev/null
}

condition_params()
{
    local cond="$1"

    case "$cond" in
        BASE)
            echo "NONE NONE 0"
            ;;
        L0)
            echo "O0 LOW $LOW_PACE_US"
            ;;
        H0)
            echo "O0 HIGH $HIGH_PACE_US"
            ;;
        L100)
            echo "O100 LOW $LOW_PACE_US"
            ;;
        H100)
            echo "O100 HIGH $HIGH_PACE_US"
            ;;
        *)
            echo "[ERROR] unknown condition: $cond" >&2
            exit 1
            ;;
    esac
}

run_one()
{
    local block="$1"
    local pos="$2"
    local cond="$3"

    local rundir="$OUTDIR/block${block}_pos${pos}_${cond}"
    mkdir -p "$rundir"

    local class activity pace
    read -r class activity pace < <(condition_params "$cond")

    local aggr_groups=""
    local aggr_seed=""
    if [[ "$class" == "O0" ]]; then
        aggr_groups="$O0_GROUPS"
        aggr_seed=2026090202
    elif [[ "$class" == "O100" ]]; then
        aggr_groups="$O100_GROUPS"
        aggr_seed=2026090206
    fi

    echo
    echo "============================================================"
    echo "C1 block=$block position=$pos condition=$cond"
    echo "============================================================"

    hard_disable_damon

    if [[ "$(sudo cat "$TRACE/events/damon/damon_aggregated/enable")" != "0" ]]; then
        echo "[ERROR] tracepoint is not OFF" >&2
        exit 1
    fi

    if [[ "$(sudo cat "$ROOT/kdamonds/nr_kdamonds")" != "0" ]]; then
        echo "[ERROR] nr_kdamonds is not zero" >&2
        exit 1
    fi

    # ---------------------------
    # Launch fresh victim
    # ---------------------------
    sudo "$W" \
        --name victim \
        --role victim \
        --groups "$VICTIM_GROUPS" \
        --pool-mib "$POOL_MIB" \
        --hot-pages-total "$HOT_PAGES" \
        --duration "$WORKLOAD_DURATION" \
        --streams 1 \
        --pace-us 0 \
        --cpu "$VICTIM_CPU" \
        --seed 2026090201 \
        >"$rundir/victim.out" 2>"$rundir/victim.err" &
    local vlauncher=$!

    # ---------------------------
    # Launch fresh aggressor if needed
    # ---------------------------
    local alauncher=""
    if [[ "$cond" != "BASE" ]]; then
        sudo "$W" \
            --name aggressor \
            --role aggressor \
            --groups "$aggr_groups" \
            --pool-mib "$POOL_MIB" \
            --hot-pages-total "$HOT_PAGES" \
            --duration "$WORKLOAD_DURATION" \
            --streams 16 \
            --pace-us "$pace" \
            --cpu "$AGGR_CPU" \
            --seed "$aggr_seed" \
            >"$rundir/aggressor.out" 2>"$rundir/aggressor.err" &
        alauncher=$!
    fi

    wait_ready "$rundir/victim.err"
    local vpid
    vpid=$(extract_pid "$rundir/victim.err")
    [[ -n "$vpid" ]] || { echo "[ERROR] missing victim pid" >&2; exit 1; }
    ACTIVE_VPID="$vpid"

    local apid=""
    if [[ "$cond" != "BASE" ]]; then
        wait_ready "$rundir/aggressor.err"
        apid=$(extract_pid "$rundir/aggressor.err")
        [[ -n "$apid" ]] || { echo "[ERROR] missing aggressor pid" >&2; exit 1; }
        ACTIVE_APID="$apid"
    fi

    if ! sudo kill -0 "$vpid" 2>/dev/null; then
        echo "[ERROR] victim not alive: $vpid" >&2
        exit 1
    fi
    if [[ -n "$apid" ]] && ! sudo kill -0 "$apid" 2>/dev/null; then
        echo "[ERROR] aggressor not alive: $apid" >&2
        exit 1
    fi

    grep -m1 '^READY ' "$rundir/victim.err"
    [[ -n "$apid" ]] && grep -m1 '^READY ' "$rundir/aggressor.err"

    # Start active phases as close together as possible.
    if [[ -n "$apid" ]]; then
        sudo kill -USR1 "$vpid" "$apid"
    else
        sudo kill -USR1 "$vpid"
    fi

    # Wait for both to confirm start.
    for _ in $(seq 1 300); do
        grep -q '^RESUMED ' "$rundir/victim.err" 2>/dev/null && break
        sleep 0.1
    done
    grep -m1 '^RESUMED ' "$rundir/victim.err"

    if [[ -n "$apid" ]]; then
        for _ in $(seq 1 300); do
            grep -q '^RESUMED ' "$rundir/aggressor.err" 2>/dev/null && break
            sleep 0.1
        done
        grep -m1 '^RESUMED ' "$rundir/aggressor.err"
    fi

    sleep "$MEASURE_SEC"

    # Stop both at the same boundary as closely as possible.
    if [[ -n "$apid" ]]; then
        sudo kill -TERM "$vpid" "$apid" 2>/dev/null || true
    else
        sudo kill -TERM "$vpid" 2>/dev/null || true
    fi

    set +e
    wait "$vlauncher"
    local vrc=$?
    if [[ -n "$alauncher" ]]; then
        wait "$alauncher"
        local arc=$?
    else
        local arc=0
    fi
    set -e

    ACTIVE_VPID=""
    ACTIVE_APID=""

    if (( vrc != 0 )); then
        echo "[ERROR] victim rc=$vrc" >&2
        tail -60 "$rundir/victim.err" >&2 || true
        exit 1
    fi
    if (( arc != 0 )); then
        echo "[ERROR] aggressor rc=$arc" >&2
        tail -60 "$rundir/aggressor.err" >&2 || true
        exit 1
    fi

    local vend
    vend=$(grep -m1 '^END ' "$rundir/victim.err" || true)
    [[ -n "$vend" ]] || { echo "[ERROR] missing victim END" >&2; exit 1; }
    echo "$vend"

    local vmops
    vmops=$(sed -n 's/.*Mops_per_sec=\([0-9.]*\).*/\1/p' "$rundir/victim.err" | tail -1)
    [[ -n "$vmops" ]] || { echo "[ERROR] missing victim Mops" >&2; exit 1; }

    local vns
    vns=$(awk -v m="$vmops" 'BEGIN {printf "%.6f", 1000.0/m}')

    local amops=""
    if [[ -n "$apid" ]]; then
        local aend
        aend=$(grep -m1 '^END ' "$rundir/aggressor.err" || true)
        [[ -n "$aend" ]] || { echo "[ERROR] missing aggressor END" >&2; exit 1; }
        echo "$aend"

        amops=$(sed -n 's/.*Mops_per_sec=\([0-9.]*\).*/\1/p' "$rundir/aggressor.err" | tail -1)
        [[ -n "$amops" ]] || { echo "[ERROR] missing aggressor Mops" >&2; exit 1; }
    fi

    local vpfn=0
    if grep -q 'PFN_VERIFY_ALL,name=victim,moved=0,unreadable=0' "$rundir/victim.out"; then
        vpfn=1
    fi

    local apfn=""
    if [[ -n "$apid" ]]; then
        apfn=0
        if grep -q 'PFN_VERIFY_ALL,name=aggressor,moved=0,unreadable=0' "$rundir/aggressor.out"; then
            apfn=1
        fi
    fi

    grep 'PFN_VERIFY_ALL' "$rundir/victim.out" || true
    [[ -n "$apid" ]] && grep 'PFN_VERIFY_ALL' "$rundir/aggressor.out" || true

    if (( vpfn != 1 )); then
        echo "[ERROR] victim PFN verification failed" >&2
        exit 1
    fi
    if [[ -n "$apid" && "$apfn" != "1" ]]; then
        echo "[ERROR] aggressor PFN verification failed" >&2
        exit 1
    fi

    echo "RESULT,block=$block,pos=$pos,condition=$cond,victim_ns=$vns,victim_mops=$vmops,aggr_mops=${amops:-NA},pace_us=$pace"

    echo "$block,$pos,$cond,$vpid,${apid:-},$vmops,$vns,${amops:-},$pace,$vpfn,${apfn:-}" \
        >> "$OUTDIR/results.csv"

    sleep 2
}

echo "============================================================"
echo "Step C1: DAMON-free victim-harm ground truth"
echo "============================================================"
echo "measure_sec=$MEASURE_SEC"
echo "victim_cpu=$VICTIM_CPU"
echo "aggressor_cpu=$AGGR_CPU"
echo "low_pace_us=$LOW_PACE_US"
echo "tracepoint=OFF"
echo "nr_kdamonds=0"
echo "============================================================"

for block in 1 2 3 4 5; do
    arr_name="BLOCK${block}[@]"
    order=("${!arr_name}")

    echo
    echo "################ BLOCK $block: ${order[*]} ################"

    pos=0
    for cond in "${order[@]}"; do
        pos=$((pos + 1))
        run_one "$block" "$pos" "$cond"
    done
done

python3 - "$OUTDIR/results.csv" <<'PY'
import csv
import statistics
import sys
from collections import defaultdict

path = sys.argv[1]

rows = []
with open(path, newline="") as f:
    for r in csv.DictReader(f):
        r["block"] = int(r["block"])
        r["position"] = int(r["position"])
        r["victim_ns_per_read"] = float(r["victim_ns_per_read"])
        r["victim_mops"] = float(r["victim_mops"])
        r["aggr_mops"] = float(r["aggr_mops"]) if r["aggr_mops"].strip() else None
        rows.append(r)

by_cond = defaultdict(list)
by_block = defaultdict(dict)

for r in rows:
    by_cond[r["condition"]].append(r)
    by_block[r["block"]][r["condition"]] = r

def mean_sd(vals):
    m = statistics.mean(vals)
    sd = statistics.stdev(vals) if len(vals) >= 2 else 0.0
    return m, sd

print()
print("============================================================")
print("STEPC_C1_GROUNDTRUTH_SUMMARY")
print("============================================================")

order = ["BASE", "L0", "H0", "L100", "H100"]

for cond in order:
    vals = [r["victim_ns_per_read"] for r in by_cond[cond]]
    m, sd = mean_sd(vals)

    aggr = [r["aggr_mops"] for r in by_cond[cond] if r["aggr_mops"] is not None]
    if aggr:
        am, asd = mean_sd(aggr)
        print(f"{cond}_N={len(vals)}")
        print(f"{cond}_VICTIM_NS_MEAN={m:.6f}")
        print(f"{cond}_VICTIM_NS_SD={sd:.6f}")
        print(f"{cond}_AGGR_MOPS_MEAN={am:.6f}")
        print(f"{cond}_AGGR_MOPS_SD={asd:.6f}")
    else:
        print(f"{cond}_N={len(vals)}")
        print(f"{cond}_VICTIM_NS_MEAN={m:.6f}")
        print(f"{cond}_VICTIM_NS_SD={sd:.6f}")

base_mean = statistics.mean(
    [r["victim_ns_per_read"] for r in by_cond["BASE"]]
)

print()
print("GLOBAL_MEAN_BASELINE_HARM")
for cond in ["L0", "H0", "L100", "H100"]:
    m = statistics.mean([r["victim_ns_per_read"] for r in by_cond[cond]])
    harm = (m - base_mean) / base_mean * 100.0
    print(f"{cond}_HARM_VS_GLOBAL_BASE_PCT={harm:.6f}")

print()
print("BLOCK_NORMALIZED_HARM")
block_harms = defaultdict(list)

for b in sorted(by_block):
    base = by_block[b]["BASE"]["victim_ns_per_read"]
    print(f"BLOCK={b},BASE_NS={base:.6f}", end="")
    for cond in ["L0", "H0", "L100", "H100"]:
        ns = by_block[b][cond]["victim_ns_per_read"]
        harm = (ns - base) / base * 100.0
        block_harms[cond].append(harm)
        print(f",{cond}_HARM_PCT={harm:.6f}", end="")
    print()

print()
print("BLOCK_NORMALIZED_HARM_SUMMARY")
for cond in ["L0", "H0", "L100", "H100"]:
    m, sd = mean_sd(block_harms[cond])
    print(f"{cond}_HARM_PCT_MEAN={m:.6f}")
    print(f"{cond}_HARM_PCT_SD={sd:.6f}")

print()
print("KEY_CONTRASTS")

# Main adversarial contrast: activity points to H0; overlap points to L100.
diffs = []
for b in sorted(by_block):
    h0 = by_block[b]["H0"]["victim_ns_per_read"]
    l100 = by_block[b]["L100"]["victim_ns_per_read"]
    diffs.append(l100 - h0)

dm, dsd = mean_sd(diffs)
wins = sum(d > 0 for d in diffs)

print(f"L100_MINUS_H0_NS_MEAN={dm:.6f}")
print(f"L100_MINUS_H0_NS_SD={dsd:.6f}")
print(f"L100_GT_H0_BLOCKS={wins}/{len(diffs)}")

# Effects of overlap at fixed activity.
low_overlap_effect = []
high_overlap_effect = []
for b in sorted(by_block):
    low_overlap_effect.append(
        by_block[b]["L100"]["victim_ns_per_read"]
        - by_block[b]["L0"]["victim_ns_per_read"]
    )
    high_overlap_effect.append(
        by_block[b]["H100"]["victim_ns_per_read"]
        - by_block[b]["H0"]["victim_ns_per_read"]
    )

lm, lsd = mean_sd(low_overlap_effect)
hm, hsd = mean_sd(high_overlap_effect)
print(f"OVERLAP_EFFECT_AT_LOW_ACTIVITY_NS_MEAN={lm:.6f}")
print(f"OVERLAP_EFFECT_AT_LOW_ACTIVITY_NS_SD={lsd:.6f}")
print(f"OVERLAP_EFFECT_AT_HIGH_ACTIVITY_NS_MEAN={hm:.6f}")
print(f"OVERLAP_EFFECT_AT_HIGH_ACTIVITY_NS_SD={hsd:.6f}")

# Effects of activity at fixed overlap.
activity0 = []
activity100 = []
for b in sorted(by_block):
    activity0.append(
        by_block[b]["H0"]["victim_ns_per_read"]
        - by_block[b]["L0"]["victim_ns_per_read"]
    )
    activity100.append(
        by_block[b]["H100"]["victim_ns_per_read"]
        - by_block[b]["L100"]["victim_ns_per_read"]
    )

a0m, a0sd = mean_sd(activity0)
a100m, a100sd = mean_sd(activity100)
print(f"ACTIVITY_EFFECT_AT_0_OVERLAP_NS_MEAN={a0m:.6f}")
print(f"ACTIVITY_EFFECT_AT_0_OVERLAP_NS_SD={a0sd:.6f}")
print(f"ACTIVITY_EFFECT_AT_100_OVERLAP_NS_MEAN={a100m:.6f}")
print(f"ACTIVITY_EFFECT_AT_100_OVERLAP_NS_SD={a100sd:.6f}")

print("============================================================")
PY

echo
echo "RESULT_CSV=$OUTDIR/results.csv"
echo "STEPC_C1_COMPLETE"

trap - EXIT INT TERM

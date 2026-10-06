#!/usr/bin/env bash
set -euo pipefail

ROOT=/sys/kernel/mm/damon/admin
TRACE=/sys/kernel/tracing

W=./stepc_activity_overlap_workload
OUTDIR="${OUTDIR:-stepc_c0_pacing}"

POOL_MIB="${POOL_MIB:-160}"
HOT_PAGES="${HOT_PAGES:-16384}"
DURATION="${DURATION:-10}"
CPU="${CPU:-2}"

# Provisional low-intensity pacing.
LOW_PACE_US="${LOW_PACE_US:-500}"

VICTIM_GROUPS="24,31,12,23,16,2,10,18,30,21,26,5,11,1,29,3"
O0_GROUPS="0,4,6,7,8,9,13,14,15,17,19,20,22,25,27,28"
O100_GROUPS="$VICTIM_GROUPS"

if ! sudo -n true 2>/dev/null; then
    echo "[ERROR] sudo credential not active. Run: sudo -v" >&2
    exit 1
fi

./build_stepc_c0.sh

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# C0 must contain no DAMON monitoring and no DAMON trace output.
sudo sh -c "echo 0 > '$TRACE/events/damon/damon_aggregated/enable'"

# Best-effort stop any existing DAMON and remove configured kdamonds.
for s in "$ROOT"/kdamonds/*/state; do
    if sudo test -e "$s"; then
        echo off | sudo tee "$s" >/dev/null 2>&1 || true
    fi
done
echo 0 | sudo tee "$ROOT/kdamonds/nr_kdamonds" >/dev/null

echo "tracepoint=$(sudo cat "$TRACE/events/damon/damon_aggregated/enable")"
echo "nr_kdamonds=$(sudo cat "$ROOT/kdamonds/nr_kdamonds")"

echo "name,overlap,pacing,pace_us,mops,ns_per_op,pfn_ok" > "$OUTDIR/results.csv"

ACTIVE_PID=""

cleanup()
{
    set +e
    if [[ -n "${ACTIVE_PID:-}" ]]; then
        sudo kill -TERM "$ACTIVE_PID" 2>/dev/null || true
    fi
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
    tail -50 "$f" >&2 || true
    exit 1
}

run_one()
{
    local name="$1"
    local overlap="$2"
    local groups="$3"
    local pace="$4"
    local seed="$5"
    local tag="$6"

    local out="$OUTDIR/${name}.out"
    local err="$OUTDIR/${name}.err"

    echo
    echo "============================================================"
    echo "C0 $name overlap=${overlap}% tag=$tag pace_us=$pace"
    echo "============================================================"

    sudo "$W" \
        --name "$name" \
        --role aggressor \
        --groups "$groups" \
        --pool-mib "$POOL_MIB" \
        --hot-pages-total "$HOT_PAGES" \
        --duration "$DURATION" \
        --streams 16 \
        --pace-us "$pace" \
        --cpu "$CPU" \
        --seed "$seed" \
        >"$out" 2>"$err" &

    local launcher=$!

    wait_ready "$err"

    local pid
    pid=$(sed -n 's/^READY pid=\([0-9]*\).*/\1/p' "$err" | head -1)
    [[ -n "$pid" ]] || { echo "[ERROR] missing pid" >&2; exit 1; }

    ACTIVE_PID="$pid"

    if ! sudo kill -0 "$pid" 2>/dev/null; then
        echo "[ERROR] workload PID not alive: $pid" >&2
        exit 1
    fi

    grep -m1 '^READY ' "$err"

    sudo kill -USR1 "$pid"

    set +e
    wait "$launcher"
    local rc=$?
    set -e
    ACTIVE_PID=""

    if (( rc != 0 )); then
        echo "[ERROR] workload rc=$rc" >&2
        tail -80 "$err" >&2 || true
        exit 1
    fi

    local endline
    endline=$(grep -m1 '^END ' "$err" || true)
    [[ -n "$endline" ]] || { echo "[ERROR] missing END" >&2; exit 1; }
    echo "$endline"

    local mops
    mops=$(sed -n 's/.*Mops_per_sec=\([0-9.]*\).*/\1/p' "$err" | tail -1)
    [[ -n "$mops" ]] || { echo "[ERROR] missing Mops_per_sec" >&2; exit 1; }

    local ns
    ns=$(awk -v m="$mops" 'BEGIN {printf "%.6f", 1000.0/m}')

    local pfn_ok=0
    if grep -q "PFN_VERIFY_ALL,name=${name},moved=0,unreadable=0" "$out"; then
        pfn_ok=1
    fi
    grep 'PFN_VERIFY_ALL' "$out" || true

    if (( pfn_ok != 1 )); then
        echo "[ERROR] PFN verification failed" >&2
        exit 1
    fi

    echo "RESULT,name=$name,overlap=$overlap,tag=$tag,pace_us=$pace,Mops_per_sec=$mops,ns_per_op=$ns"
    echo "$name,$overlap,$tag,$pace,$mops,$ns,$pfn_ok" >> "$OUTDIR/results.csv"

    sleep 2
}

# Use the same shuffle seed within each overlap level so High/Low differ
# only in pacing, aside from fresh physical allocation.
run_one o0_high   0   "$O0_GROUPS"   0              2026090202 HIGH
run_one o0_low    0   "$O0_GROUPS"   "$LOW_PACE_US" 2026090202 LOW
run_one o100_low  100 "$O100_GROUPS" "$LOW_PACE_US" 2026090206 LOW
run_one o100_high 100 "$O100_GROUPS" 0              2026090206 HIGH

python3 - "$OUTDIR/results.csv" <<'PY'
import csv
import statistics
import sys

path = sys.argv[1]
rows = {}
with open(path, newline="") as f:
    for r in csv.DictReader(f):
        rows[r["name"]] = r

def m(name):
    return float(rows[name]["mops"])

o0_ratio = m("o0_low") / m("o0_high")
o100_ratio = m("o100_low") / m("o100_high")
mean_ratio = statistics.mean([o0_ratio, o100_ratio])

print()
print("============================================================")
print("STEPC_C0_PACING_SUMMARY")
print("============================================================")
print(f"O0_HIGH_MOPS={m('o0_high'):.6f}")
print(f"O0_LOW_MOPS={m('o0_low'):.6f}")
print(f"O0_LOW_OVER_HIGH={o0_ratio:.6f}")
print(f"O100_HIGH_MOPS={m('o100_high'):.6f}")
print(f"O100_LOW_MOPS={m('o100_low'):.6f}")
print(f"O100_LOW_OVER_HIGH={o100_ratio:.6f}")
print(f"MEAN_LOW_OVER_HIGH={mean_ratio:.6f}")

if 0.20 <= mean_ratio <= 0.45:
    print("PACING_CALIBRATION=GOOD")
    print("RECOMMENDATION=KEEP_CURRENT_LOW_PACE_US")
elif mean_ratio > 0.45:
    print("PACING_CALIBRATION=TOO_HIGH")
    print("RECOMMENDATION=INCREASE_LOW_PACE_US")
else:
    print("PACING_CALIBRATION=TOO_LOW")
    print("RECOMMENDATION=DECREASE_LOW_PACE_US")

print("============================================================")
PY

echo
echo "RESULT_CSV=$OUTDIR/results.csv"
echo "STEPC_C0_COMPLETE"

trap - EXIT INT TERM

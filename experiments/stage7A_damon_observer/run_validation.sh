#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKLOAD="$ROOT/bin/damon_access_pattern_workload"
OBSERVER="$ROOT/bin/damon_observer_cli"
RESULTS="$ROOT/results"
DURATION="${1:-15}"
STAMP="$(date +%Y%m%d-%H%M%S)"
WLOG="$RESULTS/stage7A_workload_${STAMP}.log"
DLOG="$RESULTS/stage7A_damon_${STAMP}.csv"

mkdir -p "$RESULTS"

if [[ ! -x "$WORKLOAD" ]]; then
    echo "missing workload binary: $WORKLOAD" >&2
    echo "build from src/: make -f Makefile.workload" >&2
    exit 1
fi
if [[ ! -x "$OBSERVER" ]]; then
    echo "missing observer binary: $OBSERVER" >&2
    exit 1
fi

"$WORKLOAD" --duration $((DURATION + 10)) >"$WLOG" 2>&1 &
WPID=$!
cleanup() {
    kill "$WPID" 2>/dev/null || true
    wait "$WPID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for _ in $(seq 1 100); do
    if grep -q '^READY,pid=' "$WLOG" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

if ! grep -q '^READY,pid=' "$WLOG"; then
    echo "workload did not become READY" >&2
    cat "$WLOG" >&2
    exit 1
fi

PID="$(sed -n 's/^READY,pid=//p' "$WLOG" | head -n1)"

echo "=== Controlled regions ==="
grep '^REGION,' "$WLOG"
echo "=== DAMON target PID: $PID ==="
echo "Observer output: $DLOG"

sudo "$OBSERVER" --duration "$DURATION" "$PID" >"$DLOG"

echo "=== Done ==="
echo "workload log: $WLOG"
echo "damon csv:    $DLOG"
echo "Ignore the first few aggregation records if setup-fault activity is visible in COLD."


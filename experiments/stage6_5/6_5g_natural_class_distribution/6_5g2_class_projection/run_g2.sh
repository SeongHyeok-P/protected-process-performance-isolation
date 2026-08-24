#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 PID [DURATION_SEC]" >&2
    exit 2
fi

PID="$1"
DURATION="${2:-30}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/results/g2_pid${PID}_$(date +%Y%m%d_%H%M%S).csv"

echo "[G2] PID=$PID duration=${DURATION}s"
echo "[G2] output=$OUT"

sudo "$ROOT/bin/damon_class_observer_cli" \
    --duration "$DURATION" \
    "$PID" | tee "$OUT"

echo
echo "[G2] summaries"
grep -E '^(SUMMARY|COMPARE|PROJECTOR_STATS)' "$OUT" || true


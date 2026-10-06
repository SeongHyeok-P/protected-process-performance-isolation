#!/usr/bin/env bash
set -Eeuo pipefail

# Final natural-workload validation:
#   3 cyclic CPU placements x 4 balanced intervention blocks = 12 blocks.
#
# Requires the already validated base experiment in the same directory:
#   run_natural_fallback_intervention.sh
#
# Placement A: BFS=2,4   CC=6,8    PR=10,12
# Placement B: CC=2,4    PR=6,8    BFS=10,12
# Placement C: PR=2,4    BFS=6,8   CC=10,12
#
# Each workload therefore occupies every CPU pair exactly once.
# BLOCKS_PER_PLACEMENT=4 is deliberate: the base script's first four
# intervention orders form a complete 4-position rotation of
# UNCTRL/T_BFS/T_CC/T_PR.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BASE_SCRIPT="${BASE_SCRIPT:-$SCRIPT_DIR/run_natural_fallback_intervention.sh}"

BLOCKS_PER_PLACEMENT="${BLOCKS_PER_PLACEMENT:-4}"
GAPBS_TRIALS="${GAPBS_TRIALS:-1000}"
LATENCY_EPOCHS="${LATENCY_EPOCHS:-4}"
MEASURE_TRAFFIC="${MEASURE_TRAFFIC:-0}"
THROTTLE_CPU_MAX="${THROTTLE_CPU_MAX:-20000 100000}"
VICTIM_CPU="${VICTIM_CPU:-14}"

STAMP="$(date +%Y%m%d_%H%M%S)"
ROTATION_ROOT="${ROTATION_ROOT:-$SCRIPT_DIR/results/placement_rotation_$STAMP}"

log() {
    printf '[%(%H:%M:%S)T] %s\n' -1 "$*"
}

die() {
    echo "[ERROR] $*" >&2
    exit 1
}

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
    die "run as root: sudo ./run_placement_rotation.sh"
fi

[[ -x "$BASE_SCRIPT" ]] || die "base script not executable: $BASE_SCRIPT"

case "$BLOCKS_PER_PLACEMENT" in
    ''|*[!0-9]*) die "BLOCKS_PER_PLACEMENT must be an integer" ;;
esac

if (( BLOCKS_PER_PLACEMENT < 1 )); then
    die "BLOCKS_PER_PLACEMENT must be >= 1"
fi

RUN_USER="${SUDO_USER:-root}"
if [[ "$RUN_USER" == "root" ]]; then
    RUN_HOME="/root"
    RUN_GROUP="$(id -gn root)"
else
    RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
    [[ -n "$RUN_HOME" ]] || die "cannot resolve home for $RUN_USER"
    RUN_GROUP="$(id -gn "$RUN_USER")"
fi

mkdir -p "$ROTATION_ROOT"
chown "$RUN_USER:$RUN_GROUP" "$ROTATION_ROOT"

MANIFEST="$ROTATION_ROOT/placements.csv"
printf 'placement,bfs_cpus,cc_cpus,pagerank_cpus,blocks,result_dir\n' > "$MANIFEST"

# candidate -> CPU pair mapping
placement_config() {
    case "$1" in
        A)
            BFS_CPUS="2,4"
            CC_CPUS="6,8"
            PR_CPUS="10,12"
            ;;
        B)
            BFS_CPUS="10,12"
            CC_CPUS="2,4"
            PR_CPUS="6,8"
            ;;
        C)
            BFS_CPUS="6,8"
            CC_CPUS="10,12"
            PR_CPUS="2,4"
            ;;
        *)
            die "unknown placement: $1"
            ;;
    esac
}

cleanup_on_error() {
    local rc=$?
    if (( rc != 0 )); then
        echo "[ERROR] placement rotation aborted (rc=$rc)" >&2
        echo "[ERROR] partial results: $ROTATION_ROOT" >&2
    fi
    exit "$rc"
}
trap cleanup_on_error ERR

log "final placement-rotation experiment"
log "root: $ROTATION_ROOT"
log "blocks/placement=$BLOCKS_PER_PLACEMENT, GAPBS_TRIALS=$GAPBS_TRIALS, LATENCY_EPOCHS=$LATENCY_EPOCHS"
log "quota='$THROTTLE_CPU_MAX', victim_cpu=$VICTIM_CPU, traffic=$MEASURE_TRAFFIC"

for placement in A B C; do
    placement_config "$placement"

    RESULT_DIR="$ROTATION_ROOT/placement_$placement"
    mkdir -p "$RESULT_DIR"
    chown "$RUN_USER:$RUN_GROUP" "$RESULT_DIR"

    printf '%s,%s,%s,%s,%s,%s\n' \
        "$placement" "$BFS_CPUS" "$CC_CPUS" "$PR_CPUS" \
        "$BLOCKS_PER_PLACEMENT" "$RESULT_DIR" >> "$MANIFEST"

    log "============================================================"
    log "PLACEMENT $placement"
    log "  BFS      -> $BFS_CPUS"
    log "  CC       -> $CC_CPUS"
    log "  PageRank -> $PR_CPUS"
    log "============================================================"

    env \
        RESULT_DIR="$RESULT_DIR" \
        BLOCKS="$BLOCKS_PER_PLACEMENT" \
        GAPBS_TRIALS="$GAPBS_TRIALS" \
        LATENCY_EPOCHS="$LATENCY_EPOCHS" \
        MEASURE_TRAFFIC="$MEASURE_TRAFFIC" \
        THROTTLE_CPU_MAX="$THROTTLE_CPU_MAX" \
        VICTIM_CPU="$VICTIM_CPU" \
        BFS_CPUS="$BFS_CPUS" \
        CC_CPUS="$CC_CPUS" \
        PR_CPUS="$PR_CPUS" \
        "$BASE_SCRIPT" \
        2>&1 | tee "$ROTATION_ROOT/placement_${placement}.console.txt"

    [[ -s "$RESULT_DIR/scores.csv" ]] || die "placement $placement missing scores.csv"
    [[ -s "$RESULT_DIR/block_rankings.csv" ]] || die "placement $placement missing block_rankings.csv"
    [[ -s "$RESULT_DIR/interventions_with_adjusted.csv" ]] || \
        die "placement $placement missing interventions_with_adjusted.csv"

    log "placement $placement complete"
done

chown -R "$RUN_USER:$RUN_GROUP" "$ROTATION_ROOT"

log "combining all placements"

sudo -u "$RUN_USER" env HOME="$RUN_HOME" \
    python3 - "$ROTATION_ROOT" <<'PY'
import csv
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

root = Path(sys.argv[1])
placements = ["A", "B", "C"]
candidates = ["bfs", "cc", "pagerank"]
metrics = ["overlap", "cpu_ratio", "faults_per_sec", "rss_mib"]

cpu_map = {
    "A": {"bfs": "2,4", "cc": "6,8", "pagerank": "10,12"},
    "B": {"bfs": "10,12", "cc": "2,4", "pagerank": "6,8"},
    "C": {"bfs": "6,8", "cc": "10,12", "pagerank": "2,4"},
}

def f(x):
    return float(x)

def mean(xs):
    return statistics.mean(xs) if xs else math.nan

def sd(xs):
    return statistics.stdev(xs) if len(xs) >= 2 else math.nan

def fmt(x, digits=9):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return ""
    return f"{x:.{digits}f}"

def rank_desc(values, eps=1e-12):
    ordered = sorted(candidates, key=lambda c: (-values[c], c))
    groups = []
    cur = [ordered[0]]
    for c in ordered[1:]:
        if abs(values[c] - values[cur[-1]]) <= eps:
            cur.append(c)
        else:
            groups.append(cur)
            cur = [c]
    groups.append(cur)
    return groups

def rank_text(groups):
    return ">".join("=".join(g) for g in groups)

def top_set(groups):
    return set(groups[0])

def pair_stats(pred, actual, eps=1e-12):
    correct = wrong = pred_tie = actual_tie = 0
    pairs = [("bfs", "cc"), ("bfs", "pagerank"), ("cc", "pagerank")]
    for a, b in pairs:
        pd = pred[a] - pred[b]
        ad = actual[a] - actual[b]
        if abs(ad) <= eps:
            actual_tie += 1
            continue
        if abs(pd) <= eps:
            pred_tie += 1
        elif pd * ad > 0:
            correct += 1
        else:
            wrong += 1
    return correct, wrong, pred_tie, actual_tie

all_blocks = []
all_signal_rows = []

for placement in placements:
    pdir = root / f"placement_{placement}"
    scores_path = pdir / "scores.csv"
    ranks_path = pdir / "block_rankings.csv"

    with scores_path.open(newline="") as fobj:
        score_rows = list(csv.DictReader(fobj))
    with ranks_path.open(newline="") as fobj:
        rank_rows = list(csv.DictReader(fobj))

    scores_by_block = defaultdict(dict)
    for row in score_rows:
        cand = row.get("candidate") or row.get("label")
        if cand not in candidates:
            raise SystemExit(f"unexpected candidate in {scores_path}: {cand!r}")
        scores_by_block[int(row["block"])][cand] = row

    for rr in rank_rows:
        block = int(rr["block"])
        if set(scores_by_block[block]) != set(candidates):
            raise SystemExit(
                f"placement {placement} block {block}: incomplete scores "
                f"{sorted(scores_by_block[block])}"
            )

        actual = {
            "bfs": f(rr["bfs_adjusted_recovery_ns"]),
            "cc": f(rr["cc_adjusted_recovery_ns"]),
            "pagerank": f(rr["pagerank_adjusted_recovery_ns"]),
        }
        actual_groups = rank_desc(actual)
        actual_order = rank_text(actual_groups)
        actual_top = top_set(actual_groups)

        record = {
            "placement": placement,
            "block": block,
            "bfs_cpus": cpu_map[placement]["bfs"],
            "cc_cpus": cpu_map[placement]["cc"],
            "pagerank_cpus": cpu_map[placement]["pagerank"],
            "actual_order": actual_order,
            "actual_topset": "=".join(sorted(actual_top)),
            "unctrl_drift_ns": f(rr["unctrl_drift_ns"]),
        }

        for cand in candidates:
            record[f"{cand}_adjusted_recovery_ns"] = actual[cand]
            sr = scores_by_block[block][cand]
            for metric in metrics + ["activity", "legacy_total"]:
                record[f"{cand}_{metric}"] = f(sr[metric])

        for metric in metrics:
            vals = {
                cand: f(scores_by_block[block][cand][metric])
                for cand in candidates
            }
            groups = rank_desc(vals)
            order = rank_text(groups)
            tops = top_set(groups)
            pc, pw, pt, at = pair_stats(vals, actual)
            record[f"{metric}_order"] = order
            record[f"{metric}_topset"] = "=".join(sorted(tops))
            record[f"{metric}_top1_hit"] = int(bool(tops & actual_top))
            record[f"{metric}_pair_correct"] = pc
            record[f"{metric}_pair_wrong"] = pw
            record[f"{metric}_pair_tie"] = pt
            all_signal_rows.append({
                "placement": placement,
                "block": block,
                "metric": metric,
                "order": order,
                "actual_order": actual_order,
                "top1_hit": int(bool(tops & actual_top)),
                "pair_correct": pc,
                "pair_wrong": pw,
                "pair_tie": pt,
                "actual_pair_tie": at,
            })

        all_blocks.append(record)

# ------------------------------------------------------------------
# Combined block table
# ------------------------------------------------------------------
block_fields = [
    "placement", "block", "bfs_cpus", "cc_cpus", "pagerank_cpus",
    "unctrl_drift_ns", "actual_order", "actual_topset",
]
for c in candidates:
    block_fields.append(f"{c}_adjusted_recovery_ns")
for c in candidates:
    for metric in ["activity", "cpu_ratio", "faults_per_sec", "rss_mib", "overlap", "legacy_total"]:
        block_fields.append(f"{c}_{metric}")
for metric in metrics:
    block_fields += [
        f"{metric}_order", f"{metric}_topset", f"{metric}_top1_hit",
        f"{metric}_pair_correct", f"{metric}_pair_wrong", f"{metric}_pair_tie",
    ]

with (root / "placement_rotation_blocks.csv").open("w", newline="") as fobj:
    w = csv.DictWriter(fobj, fieldnames=block_fields)
    w.writeheader()
    w.writerows(all_blocks)

# ------------------------------------------------------------------
# Candidate x placement summaries
# ------------------------------------------------------------------
candidate_summary_rows = []
for placement in placements + ["ALL"]:
    rows = all_blocks if placement == "ALL" else [r for r in all_blocks if r["placement"] == placement]
    for cand in candidates:
        rec = [r[f"{cand}_adjusted_recovery_ns"] for r in rows]
        j = [r[f"{cand}_overlap"] for r in rows]
        cpu = [r[f"{cand}_cpu_ratio"] for r in rows]
        faults = [r[f"{cand}_faults_per_sec"] for r in rows]
        rss = [r[f"{cand}_rss_mib"] for r in rows]
        candidate_summary_rows.append({
            "placement": placement,
            "candidate": cand,
            "n": len(rows),
            "mean_recovery_ns": fmt(mean(rec)),
            "sd_recovery_ns": fmt(sd(rec)),
            "positive_recovery_blocks": sum(x > 0 for x in rec),
            "mean_overlap": fmt(mean(j)),
            "sd_overlap": fmt(sd(j)),
            "mean_cpu_ratio": fmt(mean(cpu)),
            "sd_cpu_ratio": fmt(sd(cpu)),
            "mean_faults_per_sec": fmt(mean(faults)),
            "sd_faults_per_sec": fmt(sd(faults)),
            "mean_rss_mib": fmt(mean(rss)),
            "sd_rss_mib": fmt(sd(rss)),
        })

with (root / "placement_candidate_summary.csv").open("w", newline="") as fobj:
    fields = list(candidate_summary_rows[0])
    w = csv.DictWriter(fobj, fieldnames=fields)
    w.writeheader()
    w.writerows(candidate_summary_rows)

# ------------------------------------------------------------------
# Signal ranking summary, separately by placement and combined
# ------------------------------------------------------------------
signal_summary_rows = []
for placement in placements + ["ALL"]:
    src = all_signal_rows if placement == "ALL" else [r for r in all_signal_rows if r["placement"] == placement]
    for metric in metrics:
        rows = [r for r in src if r["metric"] == metric]
        signal_summary_rows.append({
            "placement": placement,
            "metric": metric,
            "blocks": len(rows),
            "top1_hit_blocks": sum(r["top1_hit"] for r in rows),
            "pair_correct": sum(r["pair_correct"] for r in rows),
            "pair_wrong": sum(r["pair_wrong"] for r in rows),
            "pair_tie": sum(r["pair_tie"] for r in rows),
            "decidable_pair_accuracy": fmt(
                sum(r["pair_correct"] for r in rows) /
                max(1, sum(r["pair_correct"] + r["pair_wrong"] for r in rows))
            ),
            "unique_predicted_orders": len(set(r["order"] for r in rows)),
            "orders_seen": "|".join(r["order"] for r in rows),
        })

with (root / "placement_signal_summary.csv").open("w", newline="") as fobj:
    fields = list(signal_summary_rows[0])
    w = csv.DictWriter(fobj, fieldnames=fields)
    w.writeheader()
    w.writerows(signal_summary_rows)

# ------------------------------------------------------------------
# Pairwise overlap/recovery stability.  These are descriptive paired
# differences; no arbitrary significance threshold is imposed.
# ------------------------------------------------------------------
pairs = [("bfs", "cc"), ("bfs", "pagerank"), ("cc", "pagerank")]
pair_rows = []
for placement in placements + ["ALL"]:
    rows = all_blocks if placement == "ALL" else [r for r in all_blocks if r["placement"] == placement]
    for a, b in pairs:
        jdiff = [r[f"{a}_overlap"] - r[f"{b}_overlap"] for r in rows]
        rdiff = [r[f"{a}_adjusted_recovery_ns"] - r[f"{b}_adjusted_recovery_ns"] for r in rows]
        pair_rows.append({
            "placement": placement,
            "pair": f"{a}-{b}",
            "n": len(rows),
            "mean_overlap_diff": fmt(mean(jdiff)),
            "sd_overlap_diff": fmt(sd(jdiff)),
            "overlap_positive_blocks": sum(x > 0 for x in jdiff),
            "overlap_negative_blocks": sum(x < 0 for x in jdiff),
            "mean_recovery_diff_ns": fmt(mean(rdiff)),
            "sd_recovery_diff_ns": fmt(sd(rdiff)),
            "recovery_positive_blocks": sum(x > 0 for x in rdiff),
            "recovery_negative_blocks": sum(x < 0 for x in rdiff),
        })

with (root / "placement_pair_summary.csv").open("w", newline="") as fobj:
    fields = list(pair_rows[0])
    w = csv.DictWriter(fobj, fieldnames=fields)
    w.writeheader()
    w.writerows(pair_rows)

# ------------------------------------------------------------------
# Human-readable summary
# ------------------------------------------------------------------
lines = []
lines.append("Placement-rotation natural-workload validation")
lines.append("=" * 56)
lines.append(f"total blocks: {len(all_blocks)}")
lines.append("")

for placement in placements:
    rows = [r for r in all_blocks if r["placement"] == placement]
    lines.append(f"Placement {placement}: BFS={cpu_map[placement]['bfs']} CC={cpu_map[placement]['cc']} PR={cpu_map[placement]['pagerank']}")
    actual_counts = Counter(r["actual_order"] for r in rows)
    lines.append("  actual orders: " + ", ".join(f"{k} x{v}" for k, v in actual_counts.items()))
    for metric in metrics:
        sr = [r for r in signal_summary_rows if r["placement"] == placement and r["metric"] == metric][0]
        lines.append(
            f"  {metric:14s} top1={sr['top1_hit_blocks']}/{sr['blocks']} "
            f"pair={sr['pair_correct']} correct / {sr['pair_wrong']} wrong / {sr['pair_tie']} tie"
        )
    lines.append("")

lines.append("Combined 3-placement result:")
for metric in metrics:
    sr = [r for r in signal_summary_rows if r["placement"] == "ALL" and r["metric"] == metric][0]
    lines.append(
        f"  {metric:14s} top1={sr['top1_hit_blocks']}/{sr['blocks']} "
        f"pair={sr['pair_correct']} correct / {sr['pair_wrong']} wrong / {sr['pair_tie']} tie "
        f"decidable_accuracy={sr['decidable_pair_accuracy']}"
    )

lines.append("")
lines.append("Candidate mean adjusted recovery across all placements:")
for cand in candidates:
    row = [r for r in candidate_summary_rows if r["placement"] == "ALL" and r["candidate"] == cand][0]
    lines.append(
        f"  {cand:8s} {row['mean_recovery_ns']} ± {row['sd_recovery_ns']} ns; "
        f"J={row['mean_overlap']} ± {row['sd_overlap']}"
    )

lines.append("")
lines.append("Actual Top-1 by placement:")
for placement in placements:
    rows = [r for r in all_blocks if r["placement"] == placement]
    tops = Counter(r["actual_topset"] for r in rows)
    lines.append("  " + placement + ": " + ", ".join(f"{k} x{v}" for k, v in tops.items()))

lines.append("")
lines.append("Overlap paired differences (descriptive, no hard threshold):")
for row in pair_rows:
    if row["placement"] != "ALL":
        continue
    lines.append(
        f"  {row['pair']:16s} Jdiff={row['mean_overlap_diff']} ± {row['sd_overlap_diff']}; "
        f"recovery_diff={row['mean_recovery_diff_ns']} ± {row['sd_recovery_diff_ns']} ns"
    )

lines.append("")
lines.append("Interpretation guardrails:")
lines.append("- Repeated blocks within one fixed placement are not independent 3-way random guesses.")
lines.append("- Use placement rotation to test whether Top-1/ranking follows workload identity or CPU placement.")
lines.append("- Current bounded activity score may saturate; raw cpu_ratio/faults/rss are reported separately.")
lines.append("- faults/s may reflect workload allocation/reuse behavior, not memory interference magnitude.")
lines.append("- Primary ground truth remains adjusted victim latency recovery under the same CPU quota.")
lines.append("- If passive rankings remain unstable across placements, bounded intervention-based identification is the fallback.")

summary = "\n".join(lines) + "\n"
(root / "placement_rotation_summary.txt").write_text(summary)
print(summary, end="")
PY

chown -R "$RUN_USER:$RUN_GROUP" "$ROTATION_ROOT"

log "DONE"
log "summary: $ROTATION_ROOT/placement_rotation_summary.txt"
log "blocks : $ROTATION_ROOT/placement_rotation_blocks.csv"
log "signals: $ROTATION_ROOT/placement_signal_summary.csv"
log "pairs  : $ROTATION_ROOT/placement_pair_summary.csv"

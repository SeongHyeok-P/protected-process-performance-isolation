#!/usr/bin/env python3
import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path

CANDIDATES = ["bfs", "cc", "pagerank"]
METHODS = {"activity": "activity", "overlap": "overlap", "fused": "fused"}
EPS = 1e-9


def fnum(x):
    if x is None or x == "" or str(x).upper() == "NA":
        return None
    try:
        return float(x)
    except ValueError:
        return None


def sign_cmp(a, b, eps=EPS):
    d = a - b
    if abs(d) <= eps:
        return 0
    return 1 if d > 0 else -1


def average_ranks(values):
    items = sorted(values.items(), key=lambda kv: (-kv[1], kv[0]))
    ranks = {}
    i = 0
    while i < len(items):
        j = i + 1
        while j < len(items) and abs(items[j][1] - items[i][1]) <= EPS:
            j += 1
        avg = ((i + 1) + j) / 2.0
        for k in range(i, j):
            ranks[items[k][0]] = avg
        i = j
    return ranks


def spearman(a, b):
    ra = average_ranks(a)
    rb = average_ranks(b)
    xs = [ra[c] for c in CANDIDATES]
    ys = [rb[c] for c in CANDIDATES]
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    num = sum((x-mx)*(y-my) for x, y in zip(xs, ys))
    dx = math.sqrt(sum((x-mx)**2 for x in xs))
    dy = math.sqrt(sum((y-my)**2 for y in ys))
    if dx == 0 or dy == 0:
        return None
    return num / (dx * dy)


def rank_string(values):
    items = sorted(values.items(), key=lambda kv: (-kv[1], kv[0]))
    groups = []
    i = 0
    while i < len(items):
        same = [items[i][0]]
        j = i + 1
        while j < len(items) and abs(items[j][1] - items[i][1]) <= EPS:
            same.append(items[j][0])
            j += 1
        groups.append("=".join(same))
        i = j
    return ">".join(groups)


def top_set(values):
    m = max(values.values())
    return {k for k, v in values.items() if abs(v-m) <= EPS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("result_dir", type=Path)
    args = ap.parse_args()
    d = args.result_dir

    intervention_path = d / "interventions.csv"
    score_path = d / "scores.csv"
    if not intervention_path.exists() or not score_path.exists():
        raise SystemExit("missing interventions.csv or scores.csv")

    rows = []
    with intervention_path.open(newline="") as f:
        for r in csv.DictReader(f):
            r["block"] = int(r["block"])
            for k in ["before_ns", "after_ns", "raw_recovery_ns",
                      "usage_usec_delta", "nr_throttled_delta",
                      "throttled_usec_delta", "traffic_before",
                      "traffic_after", "traffic_removed"]:
                r[k] = fnum(r.get(k))
            rows.append(r)

    scores = defaultdict(dict)
    with score_path.open(newline="") as f:
        for r in csv.DictReader(f):
            b = int(r["block"])
            cand = r["candidate"]
            scores[b][cand] = {
                "activity": float(r["activity"]),
                "overlap": float(r["overlap"]),
                "fused": float(r["fused"]),
                "cpu_score": float(r["cpu_score"]),
                "fault_score": float(r["fault_score"]),
                "rss_score": float(r["rss_score"]),
                "legacy_total": float(r["legacy_total"]),
                "cpu_ratio": float(r["cpu_ratio"]),
                "faults_per_sec": float(r["faults_per_sec"]),
                "rss_mib": float(r["rss_mib"]),
            }

    by_block = defaultdict(list)
    for r in rows:
        by_block[r["block"]].append(r)

    block_out = []
    method_stats = {
        m: dict(blocks=0, unique_top1_correct=0, top_set_contains=0,
                pair_correct=0, pair_wrong=0, pair_tie=0,
                actual_pair_tie=0, spearmans=[], orders=[])
        for m in METHODS
    }
    cand_recoveries = defaultdict(list)

    for b in sorted(by_block):
        br = by_block[b]
        unctrl = [r for r in br if r["condition"] == "UNCTRL"]
        if len(unctrl) != 1 or unctrl[0]["raw_recovery_ns"] is None:
            print(f"[WARN] block {b}: missing usable UNCTRL row")
            continue
        drift = unctrl[0]["raw_recovery_ns"]

        actual = {}
        for c in CANDIDATES:
            rr = [r for r in br if r["target"] == c]
            if len(rr) != 1 or rr[0]["raw_recovery_ns"] is None:
                break
            adjusted = rr[0]["raw_recovery_ns"] - drift
            actual[c] = adjusted
            rr[0]["adjusted_recovery_ns"] = adjusted
            cand_recoveries[c].append(adjusted)
        if len(actual) != len(CANDIDATES):
            print(f"[WARN] block {b}: incomplete intervention rows")
            continue
        if set(scores.get(b, {})) != set(CANDIDATES):
            print(f"[WARN] block {b}: incomplete score rows")
            continue

        actual_order = rank_string(actual)
        actual_top = top_set(actual)
        outrow = {"block": b, "unctrl_drift_ns": drift, "actual_order": actual_order}
        for c in CANDIDATES:
            outrow[f"{c}_adjusted_recovery_ns"] = actual[c]

        for method, field in METHODS.items():
            pred = {c: scores[b][c][field] for c in CANDIDATES}
            pred_order = rank_string(pred)
            pred_top = top_set(pred)
            s = method_stats[method]
            s["blocks"] += 1
            s["orders"].append(pred_order)
            if len(pred_top) == 1 and len(actual_top) == 1 and pred_top == actual_top:
                s["unique_top1_correct"] += 1
            if pred_top & actual_top:
                s["top_set_contains"] += 1

            pair_correct = pair_wrong = pair_tie = actual_tie = 0
            for i in range(len(CANDIDATES)):
                for j in range(i + 1, len(CANDIDATES)):
                    a, c = CANDIDATES[i], CANDIDATES[j]
                    ts = sign_cmp(actual[a], actual[c])
                    ps = sign_cmp(pred[a], pred[c])
                    if ts == 0:
                        actual_tie += 1
                    elif ps == 0:
                        pair_tie += 1
                    elif ts == ps:
                        pair_correct += 1
                    else:
                        pair_wrong += 1
            s["pair_correct"] += pair_correct
            s["pair_wrong"] += pair_wrong
            s["pair_tie"] += pair_tie
            s["actual_pair_tie"] += actual_tie

            rho = spearman(actual, pred)
            if rho is not None:
                s["spearmans"].append(rho)
            outrow[f"{method}_order"] = pred_order
            outrow[f"{method}_topset"] = "=".join(sorted(pred_top))
            outrow[f"{method}_pair_correct"] = pair_correct
            outrow[f"{method}_pair_wrong"] = pair_wrong
            outrow[f"{method}_pair_tie"] = pair_tie
            outrow[f"{method}_spearman"] = "" if rho is None else rho

        block_out.append(outrow)

    intervention_fields = list(rows[0].keys()) if rows else []
    if "adjusted_recovery_ns" not in intervention_fields:
        intervention_fields.append("adjusted_recovery_ns")
    with (d / "interventions_with_adjusted.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=intervention_fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    if block_out:
        with (d / "block_rankings.csv").open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(block_out[0].keys()))
            w.writeheader()
            w.writerows(block_out)

    method_rows = []
    for method, s in method_stats.items():
        decidable = s["pair_correct"] + s["pair_wrong"]
        method_rows.append({
            "method": method,
            "blocks": s["blocks"],
            "unique_top1_correct": s["unique_top1_correct"],
            "top_set_contains_actual": s["top_set_contains"],
            "pair_correct": s["pair_correct"],
            "pair_wrong": s["pair_wrong"],
            "prediction_ties": s["pair_tie"],
            "actual_pair_ties": s["actual_pair_tie"],
            "decidable_pair_accuracy": "" if decidable == 0 else s["pair_correct"] / decidable,
            "mean_spearman": "" if not s["spearmans"] else statistics.mean(s["spearmans"]),
            "unique_order_count": len(set(s["orders"])),
            "orders_seen": "|".join(s["orders"]),
        })

    with (d / "method_summary.csv").open("w", newline="") as f:
        fields = list(method_rows[0].keys()) if method_rows else ["method"]
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(method_rows)

    cand_rows = []
    for c in CANDIDATES:
        vals = cand_recoveries[c]
        cand_rows.append({
            "candidate": c,
            "n": len(vals),
            "mean_adjusted_recovery_ns": "" if not vals else statistics.mean(vals),
            "sd_adjusted_recovery_ns": "" if len(vals) < 2 else statistics.stdev(vals),
            "positive_blocks": sum(v > 0 for v in vals),
            "values": "|".join(f"{v:.6f}" for v in vals),
        })
    with (d / "candidate_summary.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(cand_rows[0].keys()))
        w.writeheader()
        w.writerows(cand_rows)

    activity_sat = 0
    activity_blocks = 0
    for _, candmap in scores.items():
        if set(candmap) == set(CANDIDATES):
            activity_blocks += 1
            vals = [candmap[c]["activity"] for c in CANDIDATES]
            if max(vals) - min(vals) <= EPS:
                activity_sat += 1

    lines = [
        "Natural-workload fallback intervention summary",
        "=" * 52,
        f"valid ranked blocks: {len(block_out)}",
        f"activity exact-tie blocks: {activity_sat}/{activity_blocks}",
        "",
        "Candidate adjusted recovery (target raw recovery - UNCTRL drift):",
    ]
    for r in cand_rows:
        lines.append(
            f"  {r['candidate']:8s} n={r['n']} mean={r['mean_adjusted_recovery_ns']} "
            f"sd={r['sd_adjusted_recovery_ns']} positive={r['positive_blocks']}"
        )
    lines += ["", "Prediction methods:"]
    for r in method_rows:
        lines.append(
            f"  {r['method']:8s} unique_top1={r['unique_top1_correct']}/{r['blocks']} "
            f"topset_contains={r['top_set_contains_actual']}/{r['blocks']} "
            f"pair={r['pair_correct']} correct / {r['pair_wrong']} wrong / "
            f"{r['prediction_ties']} pred-tie, mean_rho={r['mean_spearman']}, "
            f"unique_orders={r['unique_order_count']}"
        )
    lines += [
        "",
        "Interpretation guardrails:",
        "- Primary ground truth is victim recovery under the same CPU-quota intervention.",
        "- UNCTRL before/after change is block-local drift and is subtracted.",
        "- Traffic counters are diagnostic; recovery/traffic is not the primary endpoint.",
        "- If activity is tied/saturated, that is evidence against an activity-only fallback under this implementation.",
        "- If activity works, switching to activity-only when spatial profiles are not distinguishable becomes supportable.",
        "- If passive rankings fail, short intervention-based identification is the defensible fallback.",
    ]
    (d / "summary.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()

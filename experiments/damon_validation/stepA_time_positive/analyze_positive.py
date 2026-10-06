#!/usr/bin/env python3
import sys, math, statistics, re
from pathlib import Path

if len(sys.argv) != 3:
    print(f"usage: {sys.argv[0]} WORKLOAD_OUT OBSERVER_LOG", file=sys.stderr)
    sys.exit(2)

wl = Path(sys.argv[1]).read_text(errors='replace').splitlines()
ob = Path(sys.argv[2]).read_text(errors='replace').splitlines()
selected_counts = {}
pool_counts = {}
pfn_verify = None
for l in wl:
    if l.startswith('GROUND_TRUTH_GROUP,'):
        p = l.split(',')
        selected_counts[int(p[1])] = int(p[3])
    elif l.startswith('POOL_GROUP,'):
        p = l.split(',')
        pool_counts[int(p[1])] = int(p[2])
    elif l.startswith('PFN_VERIFY_ALL,'):
        pfn_verify = l

if not selected_counts:
    print('ERROR:no GROUND_TRUTH_GROUP rows', file=sys.stderr)
    sys.exit(1)
if not pool_counts:
    print('ERROR:no POOL_GROUP rows', file=sys.stderr)
    sys.exit(1)

sel = set(selected_counts)
k = len(sel)
group_count = max(max(pool_counts), max(sel)) + 1
selected_total = sum(selected_counts.values())
pool_total = sum(pool_counts.values())
expected = [0.0] * group_count
pool_profile = [0.0] * group_count
for g, c in selected_counts.items():
    expected[g] = c / selected_total
for g, c in pool_counts.items():
    pool_profile[g] = c / pool_total
pool_selected_mass = sum(pool_profile[g] for g in sel)

complete = set()
for l in ob:
    if l.startswith('WINDOW_TIME_META,'):
        m = re.match(r'WINDOW_TIME_META,(\d+),.*complete=(\d+)', l)
        if m and int(m.group(2)) == 1:
            complete.add(int(m.group(1)))

H = {}
for l in ob:
    if l.startswith('WINDOW_GROUP,'):
        p = l.split(',')
        w = int(p[1]); kind = p[4]; g = int(p[5]); val = float(p[7]) / 100.0
        H.setdefault((w, kind), {})[g] = val

def normalized_vec(h):
    n = max(group_count, max(h.keys(), default=group_count-1) + 1)
    a = [h.get(i, 0.0) for i in range(n)]
    s = sum(a)
    return [x/s for x in a] if s else a

def compare(a, b):
    n = max(len(a), len(b))
    aa = a + [0.0] * (n-len(a)); bb = b + [0.0] * (n-len(b))
    tv = .5 * sum(abs(x-y) for x, y in zip(aa, bb))
    mn = sum(min(x,y) for x,y in zip(aa,bb)); mx = sum(max(x,y) for x,y in zip(aa,bb))
    jac = mn/mx if mx else 0.0
    dot = sum(x*y for x,y in zip(aa,bb))
    na = math.sqrt(sum(x*x for x in aa)); nb = math.sqrt(sum(x*x for x in bb))
    cos = dot/(na*nb) if na and nb else 0.0
    return tv, jac, cos

def activity_metric(h):
    a = normalized_vec(h)
    exp = expected + [0.0] * max(0, len(a)-len(expected))
    mass = sum(a[g] for g in sel if g < len(a))
    tv, jac, cos = compare(a, exp)
    top = sorted(range(len(a)), key=lambda g: a[g], reverse=True)[:k]
    hits = sum(g in sel for g in top)
    return mass, tv, jac, cos, hits

print(f'GROUND_TRUTH,groups={k},selected_pages={selected_total},pool_pages={pool_total},pool_selected_mass_pct={100*pool_selected_mass:.6f}')
print('GROUND_TRUTH_SELECTED,' + ','.join(str(g) for g in sorted(sel)))
if pfn_verify:
    print(pfn_verify)

print('POSITIVE_SUMMARY_HEADER,window,kind,selected_mass_pct,tv_to_selected_expected,jaccard_to_selected_expected,cosine_to_selected_expected,topk_hits,k')
rows = {}
spatial_truth_rows = []
for w in sorted(complete):
    for kind in ('SPATIAL', 'WEIGHTED', 'WEIGHTED_NORM'):
        if (w, kind) not in H:
            continue
        m = activity_metric(H[(w, kind)])
        rows.setdefault(kind, []).append(m)
        print(f'POSITIVE_SUMMARY,{w},{kind},{100*m[0]:.6f},{m[1]:.6f},{m[2]:.6f},{m[3]:.6f},{m[4]},{k}')
        if kind == 'SPATIAL':
            a = normalized_vec(H[(w, kind)])
            tv, jac, cos = compare(a, pool_profile)
            spatial_truth_rows.append((tv, jac, cos))
            print(f'SPATIAL_TRUTH,{w},tv={tv:.6f},jaccard={jac:.6f},cosine={cos:.6f}')

print('POSITIVE_MEAN_HEADER,kind,windows,selected_mass_pct_mean,tv_mean,jaccard_mean,cosine_mean,topk_hits_mean,k')
for kind, rr in rows.items():
    print(f'POSITIVE_MEAN,{kind},{len(rr)},{100*statistics.mean(x[0] for x in rr):.6f},'
          f'{statistics.mean(x[1] for x in rr):.6f},{statistics.mean(x[2] for x in rr):.6f},'
          f'{statistics.mean(x[3] for x in rr):.6f},{statistics.mean(x[4] for x in rr):.3f},{k}')

if spatial_truth_rows:
    print(f'SPATIAL_TRUTH_MEAN,windows={len(spatial_truth_rows)},'
          f'tv={statistics.mean(x[0] for x in spatial_truth_rows):.6f},'
          f'jaccard={statistics.mean(x[1] for x in spatial_truth_rows):.6f},'
          f'cosine={statistics.mean(x[2] for x in spatial_truth_rows):.6f}')

for kind in ('WEIGHTED', 'WEIGHTED_NORM'):
    if kind in rows:
        am = statistics.mean(x[0] for x in rows[kind])
        print(f'ENRICHMENT_VS_POOL,{kind},selected_mass_ratio={am/pool_selected_mass if pool_selected_mass else 0:.6f}')
        if 'SPATIAL' in rows:
            sm = statistics.mean(x[0] for x in rows['SPATIAL'])
            print(f'ENRICHMENT_VS_OBSERVER_SPATIAL,{kind},selected_mass_ratio={am/sm if sm else 0:.6f}')

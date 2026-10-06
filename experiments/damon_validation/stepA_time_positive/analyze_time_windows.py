#!/usr/bin/env python3
import re, sys, statistics
from pathlib import Path

if len(sys.argv) != 2:
    print(f"usage: {sys.argv[0]} OBSERVER_LOG", file=sys.stderr)
    sys.exit(2)

p = Path(sys.argv[1])
lines = p.read_text(errors='replace').splitlines()
deltas = []
rel_lags = []
emit_spans = []
callback_spans = []
windows = []
neg = []
parse = None
boundary = None
warmup_drops = 0
warmup_ms = None
window_ms = None
sync_ts = None
warmup_end_ts = None

for ln in lines:
    if ln.startswith('# window_mode=kernel_trace_timestamp'):
        m = re.search(r'window_ms=(\d+) warmup_ms=(\d+)', ln)
        if m:
            window_ms = int(m.group(1))
            warmup_ms = int(m.group(2))
    elif ln.startswith('AGG_SYNC,'):
        m1 = re.search(r'trace_ts_ns=(\d+)', ln)
        m2 = re.search(r'warmup_end_ts_ns=(\d+)', ln)
        if m1: sync_ts = int(m1.group(1))
        if m2: warmup_end_ts = int(m2.group(1))
    elif ln.startswith('AGG_WARMUP_DROP,'):
        warmup_drops += 1
    elif ln.startswith('AGG_COMPLETE,'):
        m = re.search(r'delta_prev_us=(\d+)', ln)
        if m and int(m.group(1)) > 0:
            deltas.append(int(m.group(1)))
        lm = re.search(r'callback_minus_trace_rel_us=(-?\d+)', ln)
        em = re.search(r'trace_emit_span_us=(\d+)', ln)
        cm = re.search(r'callback_span_us=(\d+)', ln)
        if lm: rel_lags.append(int(lm.group(1)))
        if em: emit_spans.append(int(em.group(1)))
        if cm: callback_spans.append(int(cm.group(1)))
    elif ln.startswith('WINDOW_TIME_META,'):
        m = re.match(
            r'WINDOW_TIME_META,(\d+),trace_start_ns=(\d+),trace_end_ns=(\d+),'
            r'sync_rel_start_ms=(\d+),sync_rel_end_ms=(\d+),aggregations=(\d+),complete=(\d+)',
            ln)
        if m:
            windows.append(tuple(map(int, m.groups())))
    elif ln.startswith('WINDOW_COMPARE,') and ',WEIGHTED_NORM,SPATIAL,' in ln:
        parts = ln.split(',')
        neg.append((int(parts[1]), float(parts[6]), float(parts[7]), float(parts[8])))
    elif ln.startswith('# OBSERVER_PARSE_STATS'):
        parse = ln
    elif ln.startswith('# TIME_BOUNDARY_SUMMARY'):
        boundary = ln

print('TIME_VALIDATION')
if deltas:
    print(f'agg_trace_delta_us count={len(deltas)} mean={statistics.mean(deltas):.3f} '
          f'median={statistics.median(deltas):.3f} min={min(deltas)} max={max(deltas)}')
else:
    print('agg_trace_delta_us count=0')

if rel_lags:
    print(f'callback_minus_trace_rel_us count={len(rel_lags)} mean={statistics.mean(rel_lags):.3f} '
          f'median={statistics.median(rel_lags):.3f} min={min(rel_lags)} max={max(rel_lags)}')
    if len(rel_lags) >= 2:
        print(f'callback_minus_trace_rel_growth_us={rel_lags[-1]-rel_lags[0]}')
if emit_spans:
    print(f'trace_emit_span_us mean={statistics.mean(emit_spans):.3f} max={max(emit_spans)}')
if callback_spans:
    print(f'callback_span_us mean={statistics.mean(callback_spans):.3f} max={max(callback_spans)}')

checks = []
if sync_ts is not None and warmup_end_ts is not None and warmup_ms is not None:
    warm_width_ms = (warmup_end_ts - sync_ts) / 1e6
    ok = abs(warm_width_ms - warmup_ms) < 1e-9
    checks.append(ok)
    print(f'warmup_trace_width_ms={warm_width_ms:.3f} configured={warmup_ms} check={"PASS" if ok else "FAIL"}')

for w in windows:
    idx, ts0, ts1, r0, r1, aggs, complete = w
    width_ms = (ts1 - ts0) / 1e6
    width_ok = window_ms is None or abs(width_ms - window_ms) < 1e-9
    if complete:
        checks.append(width_ok)
    print(f'window={idx} trace_width_ms={width_ms:.3f} sync_rel={r0}-{r1}ms '
          f'aggregations={aggs} complete={complete} width_check={"PASS" if width_ok else "FAIL"}')

if windows and warmup_ms is not None:
    first = windows[0]
    first_ok = first[3] >= warmup_ms
    checks.append(first_ok)
    print(f'first_measured_window_after_warmup={"PASS" if first_ok else "FAIL"} '
          f'first_sync_rel_start_ms={first[3]} warmup_ms={warmup_ms}')

if neg:
    complete_ids = {w[0] for w in windows if w[-1] == 1}
    good = [x for x in neg if x[0] in complete_ids]
    if good:
        tv = [x[1] for x in good]
        jac = [x[2] for x in good]
        cos = [x[3] for x in good]
        print(f'negative_complete_windows={len(good)} tv_mean={statistics.mean(tv):.6f} '
              f'tv_max={max(tv):.6f} jaccard_mean={statistics.mean(jac):.6f} '
              f'cosine_mean={statistics.mean(cos):.6f}')

print(f'warmup_dropped_aggregations={warmup_drops}')
if parse:
    print(parse)
    m = re.search(r'parse_fail=(\d+)', parse)
    if m:
        checks.append(int(m.group(1)) == 0)
if boundary:
    print(boundary)
    m = re.search(r'synced=(\d+)', boundary)
    if m:
        checks.append(int(m.group(1)) == 1)

if checks:
    print('TIME_STRUCTURE_CHECK=' + ('PASS' if all(checks) else 'FAIL'))

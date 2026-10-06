#!/usr/bin/env python3
import argparse, csv, math, re, statistics
from collections import defaultdict, Counter
from pathlib import Path

PAGE = 4096
N_GROUPS = 32
NORM_SCALE = 1_000_000
LINE_RE = re.compile(
    r'(?P<comm>\S+)-(?P<emitter>\d+)\s+\[[^]]+\].*?\s(?P<sec>\d+)\.(?P<frac>\d+):\s+'
    r'damon_aggregated:\s+target_id=(?P<tid>\d+)\s+nr_regions=(?P<nr>\d+)\s+'
    r'(?P<start>\d+)-(?P<end>\d+):\s+(?P<access>\d+)\s+(?P<age>\d+)'
)
TARGET_ORDER = ['victim','o0','o25','o50','o75','o100']


def fmt(x, n=6):
    if isinstance(x, float) and (math.isnan(x) or math.isinf(x)):
        return 'nan' if math.isnan(x) else 'inf'
    return f'{x:.{n}f}'


def mean(xs):
    return statistics.mean(xs) if xs else float('nan')


def sd(xs):
    return statistics.stdev(xs) if len(xs) > 1 else 0.0


def cv_pct(xs):
    if not xs:
        return float('nan')
    m = statistics.mean(xs)
    if m == 0:
        return float('inf')
    return 100.0 * (statistics.stdev(xs) / m if len(xs) > 1 else 0.0)


def normalize(xs):
    s = sum(xs)
    return [x / s for x in xs] if s > 0 else [0.0] * len(xs)


def cosine(a, b):
    dot = sum(x*y for x,y in zip(a,b))
    aa = math.sqrt(sum(x*x for x in a)); bb = math.sqrt(sum(y*y for y in b))
    return dot/(aa*bb) if aa > 0 and bb > 0 else float('nan')


def tv(a, b):
    return 0.5 * sum(abs(x-y) for x,y in zip(a,b))


def predicted_profile(selected, m):
    selected = set(selected)
    if len(selected) != 16:
        raise ValueError('expected exactly 16 selected groups')
    a = m / 16.0
    b = (1.0 - m) / 16.0
    return [a if g in selected else b for g in range(N_GROUPS)]


def parse_plan(path):
    out = {}
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            name = r['name']
            groups = [int(x) for x in r['groups'].split(':') if x]
            if len(groups) != 16 or len(set(groups)) != 16:
                raise ValueError(f'{name}: expected 16 unique groups')
            out[name] = {'kid': int(r['kdamond_id']), 'groups': groups}
    if set(out) != set(TARGET_ORDER):
        raise ValueError('plan must contain victim,o0,o25,o50,o75,o100')
    return out


def load_kdamond_map(path):
    out = {}
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            out[int(r['kdamond_pid'])] = {
                'kid': int(r['kdamond_id']),
                'target_pid': int(r['target_pid']),
                'name': r['name'],
            }
    return out


def load_page_map(path, selected):
    groups = []
    hot = []
    vas = []
    pfns = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            idx = int(r['page_index'])
            if idx != len(groups):
                raise ValueError(f'{path}: non-contiguous page_index at {idx}')
            vas.append(int(r['va'], 0))
            pfns.append(int(r['pfn']))
            g = int(r['group']); h = int(r['hot'])
            if not (0 <= g < N_GROUPS) or h not in (0,1):
                raise ValueError(f'{path}: invalid group/hot')
            groups.append(g); hot.append(h)
    if not groups:
        raise ValueError(f'{path}: empty page map')
    start = vas[0]
    for i,va in enumerate(vas):
        if va != start + i*PAGE:
            raise ValueError(f'{path}: VA map not contiguous at page {i}')
    selected = set(selected)
    hot_total = sum(hot)
    bad_hot = sum(1 for g,h in zip(groups,hot) if h and g not in selected)
    cold_total = len(groups) - hot_total
    cold_selected = sum(1 for g,h in zip(groups,hot) if not h and g in selected)
    if bad_hot or cold_total <= 0:
        raise ValueError(f'{path}: invalid hot membership')
    c = cold_selected / cold_total
    return {
        'groups': groups,
        'hot': hot,
        'pfns': pfns,
        'start': start,
        'end': start + len(groups)*PAGE,
        'pages': len(groups),
        'hot_total': hot_total,
        'cold_total': cold_total,
        'cold_selected': cold_selected,
        'c': c,
    }


def region_projection(pm, start, end, access):
    lo = max(start, pm['start']); hi = min(end, pm['end'])
    if access <= 0 or hi <= lo:
        return None
    first = max(0, (lo - pm['start']) // PAGE)
    last = min(pm['pages'], (hi - pm['start'] + PAGE - 1)//PAGE)
    if last <= first:
        return None
    counts = [0]*N_GROUPS
    h = 0
    pg = pm['groups']; ph = pm['hot']
    for idx in range(first, last):
        counts[pg[idx]] += 1
        h += ph[idx]
    p = last-first
    budget = access * NORM_SCALE
    contrib = [0]*N_GROUPS
    # Positive values only: C llroundl(x) == floor(x+0.5).
    for g,n in enumerate(counts):
        if n:
            contrib[g] = (budget*n + p//2)//p
    return contrib, p, h


class BurstState:
    __slots__ = ('first_ts','last_ts','count','expected','tid_ok','nr_ok','active')
    def __init__(self):
        self.reset()
    def reset(self):
        self.first_ts = None; self.last_ts = None; self.count = 0
        self.expected = None; self.tid_ok = True; self.nr_ok = True; self.active = []
    def add(self, row):
        if self.count == 0:
            self.first_ts = row['ts']; self.expected = row['nr']
        self.last_ts = row['ts']; self.count += 1
        if row['tid'] != 0: self.tid_ok = False
        if row['nr'] != self.expected: self.nr_ok = False
        if row['access'] > 0:
            self.active.append(row)
    def complete(self):
        return self.count > 0 and self.tid_ok and self.nr_ok and self.expected == self.count


def new_window():
    return {'units':[0]*N_GROUPS, 'direct_num':0.0, 'direct_den':0.0,
            'aggs':0, 'active_regions':0, 'projected_active_regions':0,
            'projected_pages':0}


def add_burst_to_window(w, active_rows, pm):
    w['aggs'] += 1
    w['active_regions'] += len(active_rows)
    for r in active_rows:
        rp = region_projection(pm, r['start'], r['end'], r['access'])
        if rp is None:
            continue
        contrib,p,h = rp
        w['projected_active_regions'] += 1
        w['projected_pages'] += p
        for g,x in enumerate(contrib):
            w['units'][g] += x
        w['direct_num'] += r['access'] * (h/p)
        w['direct_den'] += r['access']


def run_mode(a):
    plan = parse_plan(a.plan)
    km = load_kdamond_map(a.kdamond_map)
    if len(km) != 6:
        raise SystemExit('expected 6 kdamond emitters')
    meta_by_em = {}
    page_maps = {}
    for em,meta in km.items():
        name = meta['name']
        if name not in plan or meta['kid'] != plan[name]['kid']:
            raise SystemExit(f'kdamond/plan mismatch for {name}')
        pm_path = Path(a.page_map_dir) / f'{name}.pages.csv'
        page_maps[name] = load_page_map(pm_path, plan[name]['groups'])
        meta_by_em[em] = meta

    windows = {name:[new_window() for _ in range(a.profile_sec)] for name in TARGET_ORDER}
    states = {em:BurstState() for em in km}
    stats = Counter()
    tids = Counter()
    gap_ns = int(a.gap_ms*1e6)
    end_ns = a.measurement_start_ns + a.profile_sec*1_000_000_000

    def commit(em):
        st = states[em]
        if st.count == 0:
            return
        stats['bursts'] += 1
        if not st.complete():
            stats['incomplete_bursts'] += 1
            st.reset(); return
        stats['complete_bursts'] += 1
        ts = st.first_ts
        if ts is not None and a.measurement_start_ns <= ts < end_ns:
            b = int((ts-a.measurement_start_ns)//1_000_000_000)
            name = km[em]['name']
            add_burst_to_window(windows[name][b], st.active, page_maps[name])
            stats['measured_bursts'] += 1
        st.reset()

    with open(a.trace, errors='replace') as f:
        for line in f:
            if 'damon_aggregated:' not in line:
                continue
            stats['damon_lines'] += 1
            m = LINE_RE.search(line)
            if not m:
                stats['malformed'] += 1; continue
            stats['matched'] += 1
            em = int(m.group('emitter'))
            if em not in states:
                stats['unknown_emitters'] += 1; continue
            frac = m.group('frac')
            ts = int(m.group('sec'))*1_000_000_000 + int((frac+'000000000')[:9])
            row = {'ts':ts, 'tid':int(m.group('tid')), 'nr':int(m.group('nr')),
                   'start':int(m.group('start')), 'end':int(m.group('end')),
                   'access':int(m.group('access')), 'age':int(m.group('age'))}
            tids[row['tid']] += 1
            st = states[em]
            if st.count and ts - st.last_ts > gap_ns:
                commit(em)
            st.add(row)
    for em in states:
        commit(em)

    structure_ok = (stats['malformed']==0 and stats['unknown_emitters']==0 and
                    set(tids)=={0} and stats['matched']==stats['damon_lines'])

    profile_rows=[]; target_rows=[]; window_rows=[]
    print('PROFILE_GATE_RUN')
    print('TRACE_PARSE,' + ','.join([
        f'damon_lines={stats["damon_lines"]}',f'matched={stats["matched"]}',
        f'malformed={stats["malformed"]}',f'unknown_emitters={stats["unknown_emitters"]}',
        f'target_ids={":".join(str(x) for x in sorted(tids))}',
        f'bursts={stats["bursts"]}',f'complete_bursts={stats["complete_bursts"]}',
        f'incomplete_bursts={stats["incomplete_bursts"]}',f'measured_bursts={stats["measured_bursts"]}']))

    run_profiles={}
    for name in TARGET_ORDER:
        pm=page_maps[name]; selected=set(plan[name]['groups']); ws=windows[name]
        valid = all(w['aggs'] >= a.min_aggs_per_window and sum(w['units']) > 0 for w in ws)
        if not valid:
            structure_ok=False
        total_units=[0]*N_GROUPS; dnum=dden=0.0
        for w in ws:
            for g,x in enumerate(w['units']): total_units[g]+=x
            dnum += w['direct_num']; dden += w['direct_den']
        prof=normalize(total_units); run_profiles[name]=prof
        mval=sum(prof[g] for g in selected)
        cval=pm['c']; eta=(mval-cval)/(1.0-cval) if cval < 1 else float('nan')
        H=dnum/dden if dden>0 else float('nan')
        pred=predicted_profile(selected,mval)
        model_cos=cosine(prof,pred); model_tv=tv(prof,pred)
        aggs=[w['aggs'] for w in ws]
        print('RUN_PROFILE_TARGET,' + ','.join([
            f'run={a.run_id}',f'kdamond_id={plan[name]["kid"]}',f'name={name}',
            f'windows={len(ws)}',f'aggs_min={min(aggs)}',f'aggs_max={max(aggs)}',
            f'pool_pages={pm["pages"]}',f'hot_pages={pm["hot_total"]}',
            f'cold_pages={pm["cold_total"]}',f'cold_selected_pages={pm["cold_selected"]}',
            f'c_i={cval:.9f}',f'm_i={mval:.9f}',f'eta_i={eta:.9f}',f'H_i={H:.9f}',
            f'eta_minus_H={eta-H:.9f}',f'model_cosine={model_cos:.9f}',f'model_tv={model_tv:.9f}',
            f'profile_units={sum(total_units)}',f'status={"PASS" if valid else "FAIL"}']))
        target_rows.append({'run':a.run_id,'name':name,'kdamond_id':plan[name]['kid'],
                            'c_i':cval,'m_i':mval,'eta_i':eta,'H_i':H,
                            'eta_minus_H':eta-H,'model_cosine':model_cos,'model_tv':model_tv,
                            'windows':len(ws),'aggs_min':min(aggs),'aggs_max':max(aggs),
                            'profile_units':sum(total_units),'status':'PASS' if valid else 'FAIL'})
        for g in range(N_GROUPS):
            profile_rows.append({'run':a.run_id,'name':name,'group':g,'units':total_units[g],'frac':prof[g]})

        for wi,w in enumerate(ws):
            wp=normalize(w['units'])
            wm=sum(wp[g] for g in selected)
            weta=(wm-cval)/(1.0-cval) if cval<1 else float('nan')
            wH=w['direct_num']/w['direct_den'] if w['direct_den']>0 else float('nan')
            wc=cosine(wp,prof); wt=tv(wp,prof)
            print('WINDOW_PROFILE,' + ','.join([
                f'run={a.run_id}',f'name={name}',f'window={wi}',f'aggs={w["aggs"]}',
                f'm_i={wm:.9f}',f'eta_i={weta:.9f}',f'H_i={wH:.9f}',
                f'cosine_to_run_mean={wc:.9f}',f'tv_to_run_mean={wt:.9f}',
                f'active_regions={w["active_regions"]}',f'projected_active_regions={w["projected_active_regions"]}',
                f'projected_pages={w["projected_pages"]}']))
            window_rows.append({'run':a.run_id,'name':name,'window':wi,'aggs':w['aggs'],
                                'm_i':wm,'eta_i':weta,'H_i':wH,'cosine_to_run_mean':wc,
                                'tv_to_run_mean':wt,'active_regions':w['active_regions'],
                                'projected_active_regions':w['projected_active_regions'],
                                'projected_pages':w['projected_pages']})

    ms=[r['m_i'] for r in target_rows]; es=[r['eta_i'] for r in target_rows]; hs=[r['H_i'] for r in target_rows]
    print(f'RUN_HETEROGENEITY,run={a.run_id},m_range={max(ms)-min(ms):.9f},eta_range={max(es)-min(es):.9f},H_range={max(hs)-min(hs):.9f}')
    print('RUN_STRUCTURE_CHECK=' + ('PASS' if structure_ok else 'FAIL'))

    def write_csv(path, rows):
        if not rows: return
        with open(path,'w',newline='') as f:
            w=csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    write_csv(a.output_profile_csv, profile_rows)
    write_csv(a.output_target_csv, target_rows)
    write_csv(a.output_window_csv, window_rows)
    if not structure_ok:
        raise SystemExit(2)


def read_csvs(paths):
    rows=[]
    for p in paths:
        with open(p,newline='') as f: rows.extend(csv.DictReader(f))
    return rows


def aggregate_mode(a):
    pr=read_csvs(a.profile_csvs)
    tr=read_csvs(a.target_csvs)
    wr=read_csvs(a.window_csvs)
    profiles=defaultdict(dict)
    for r in pr:
        profiles[r['name']].setdefault(int(r['run']), [0.0]*N_GROUPS)[int(r['group'])]=float(r['frac'])
    targets=defaultdict(dict)
    for r in tr: targets[r['name']][int(r['run'])]=r

    expected_pairs=a.expected_runs*(a.expected_runs-1)//2
    total_pairs=passed_pairs=0; all_ok=True
    print('PROFILE_REPEAT_PAIR_HEADER,name,run_a,run_b,cosine,tv,cos_gate,tv_gate,status')
    for name in TARGET_ORDER:
        runs=sorted(profiles[name])
        if len(runs)!=a.expected_runs:
            all_ok=False
        pair_cos=[]; pair_tv=[]; local_pass=0
        for i in range(len(runs)):
            for j in range(i+1,len(runs)):
                ra,rb=runs[i],runs[j]
                c=cosine(profiles[name][ra],profiles[name][rb]); t=tv(profiles[name][ra],profiles[name][rb])
                ok=(c>=a.cos_gate and t<=a.tv_gate)
                total_pairs+=1; passed_pairs+=int(ok); local_pass+=int(ok)
                pair_cos.append(c); pair_tv.append(t)
                print(f'PROFILE_REPEAT_PAIR,{name},{ra},{rb},{c:.9f},{t:.9f},{a.cos_gate:.6f},{a.tv_gate:.6f},{"PASS" if ok else "FAIL"}')
                if not ok: all_ok=False
        rr=[targets[name][r] for r in sorted(targets[name])]
        ms=[float(x['m_i']) for x in rr]; es=[float(x['eta_i']) for x in rr]; hs=[float(x['H_i']) for x in rr]
        cs=[float(x['c_i']) for x in rr]; mc=[float(x['model_cosine']) for x in rr]; mt=[float(x['model_tv']) for x in rr]
        mcv=cv_pct(ms); ecv=cv_pct(es); hcv=cv_pct(hs)
        diag='PASS' if mcv<a.isolation_cv_gate_pct else 'FAIL'
        print('PROFILE_REPEAT_TARGET,' + ','.join([
            f'name={name}',f'runs={len(runs)}',f'pairs={len(pair_cos)}',
            f'cosine_min={min(pair_cos) if pair_cos else float("nan"):.9f}',
            f'cosine_mean={mean(pair_cos):.9f}',f'tv_max={max(pair_tv) if pair_tv else float("nan"):.9f}',
            f'tv_mean={mean(pair_tv):.9f}',f'pair_pass={local_pass}/{expected_pairs}',
            f'c_mean={mean(cs):.9f}',f'c_min={min(cs):.9f}',f'c_max={max(cs):.9f}',
            f'm_mean={mean(ms):.9f}',f'm_cv_pct={mcv:.6f}',
            f'eta_mean={mean(es):.9f}',f'eta_cv_pct={ecv:.6f}',
            f'H_mean={mean(hs):.9f}',f'H_cv_pct={hcv:.6f}',
            f'eta_minus_H_abs_mean={mean([abs(e-h) for e,h in zip(es,hs)]):.9f}',
            f'model_cosine_mean={mean(mc):.9f}',f'model_tv_mean={mean(mt):.9f}',
            f'isolation_cv_diag={diag}',
            f'status={"PASS" if local_pass==expected_pairs and len(runs)==a.expected_runs else "FAIL"}']))

    # Same-run heterogeneity: diagnostic only.
    byrun=defaultdict(list)
    for r in tr: byrun[int(r['run'])].append(r)
    for run in sorted(byrun):
        rr=byrun[run]
        ms=[float(x['m_i']) for x in rr]; es=[float(x['eta_i']) for x in rr]; hs=[float(x['H_i']) for x in rr]
        print(f'ISOLATION_HETEROGENEITY,run={run},m_range={max(ms)-min(ms):.9f},eta_range={max(es)-min(es):.9f},H_range={max(hs)-min(hs):.9f}')
        vec=','.join(f'{x["name"]}:{float(x["m_i"]):.6f}' for x in sorted(rr,key=lambda z:TARGET_ORDER.index(z['name'])))
        print(f'NEXT_J_M_VECTOR,run={run},{vec}')

    # Window stability summary is diagnostic, not the hard cross-run gate.
    byw=defaultdict(list)
    for r in wr: byw[r['name']].append(r)
    for name in TARGET_ORDER:
        rr=byw[name]
        cs=[float(x['cosine_to_run_mean']) for x in rr]; ts=[float(x['tv_to_run_mean']) for x in rr]
        print(f'WINDOW_STABILITY,name={name},windows={len(rr)},cosine_min={min(cs):.9f},cosine_mean={mean(cs):.9f},tv_max={max(ts):.9f},tv_mean={mean(ts):.9f}')

    expected_total=len(TARGET_ORDER)*expected_pairs
    if total_pairs != expected_total:
        all_ok=False
    print(f'PROFILE_REPEATABILITY_FINAL,targets=6,runs={a.expected_runs},pairs={total_pairs}/{expected_total},pair_pass={passed_pairs}/{expected_total},cos_gate={a.cos_gate:.6f},tv_gate={a.tv_gate:.6f},status={"PASS" if all_ok else "FAIL"}')
    print('PROFILE_REPEATABILITY_GATE=' + ('PASS' if all_ok else 'FAIL'))
    if not all_ok:
        raise SystemExit(3)


def main():
    p=argparse.ArgumentParser()
    sub=p.add_subparsers(dest='mode',required=True)
    r=sub.add_parser('run')
    r.add_argument('trace'); r.add_argument('--kdamond-map',required=True); r.add_argument('--page-map-dir',required=True); r.add_argument('--plan',required=True)
    r.add_argument('--measurement-start-ns',type=int,required=True); r.add_argument('--profile-sec',type=int,default=8); r.add_argument('--run-id',type=int,required=True)
    r.add_argument('--gap-ms',type=float,default=30.0); r.add_argument('--min-aggs-per-window',type=int,default=6)
    r.add_argument('--output-profile-csv',required=True); r.add_argument('--output-target-csv',required=True); r.add_argument('--output-window-csv',required=True)
    g=sub.add_parser('aggregate')
    g.add_argument('--profile-csvs',nargs='+',required=True); g.add_argument('--target-csvs',nargs='+',required=True); g.add_argument('--window-csvs',nargs='+',required=True)
    g.add_argument('--expected-runs',type=int,default=3); g.add_argument('--cos-gate',type=float,default=0.95); g.add_argument('--tv-gate',type=float,default=0.15)
    g.add_argument('--isolation-cv-gate-pct',type=float,default=10.0)
    a=p.parse_args()
    if a.mode=='run': run_mode(a)
    else: aggregate_mode(a)

if __name__=='__main__': main()

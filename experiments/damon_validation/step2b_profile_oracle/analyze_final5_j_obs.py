#!/usr/bin/env python3
import argparse, csv, math, statistics, sys
from collections import defaultdict
from pathlib import Path

N_GROUPS = 32
TARGET_ORDER = ['victim','o0','o25','o50','o75','o100']
CANDIDATES = ['o0','o25','o50','o75','o100']
EXPECTED_OVERLAP = {'o0':0, 'o25':25, 'o50':50, 'o75':75, 'o100':100}


def normalize(xs):
    s = sum(xs)
    if s <= 0:
        raise ValueError('zero-mass profile')
    return [x/s for x in xs]


def weighted_jaccard(a, b):
    inter = sum(min(x,y) for x,y in zip(a,b))
    union = sum(max(x,y) for x,y in zip(a,b))
    return inter/union if union > 0 else float('nan')


def predicted_profile(selected, m):
    ss = set(selected)
    if len(ss) != 16:
        raise ValueError('expected 16 selected groups')
    return [m/16.0 if g in ss else (1.0-m)/16.0 for g in range(N_GROUPS)]


def strict_monotonic(vals):
    return all(vals[i] < vals[i+1] for i in range(len(vals)-1))


def ranks(values):
    # Average rank for ties; ranks start at 1.
    order = sorted(range(len(values)), key=lambda i: values[i])
    out = [0.0]*len(values)
    k = 0
    while k < len(order):
        j = k+1
        while j < len(order) and values[order[j]] == values[order[k]]:
            j += 1
        avg = ((k+1) + j) / 2.0
        for p in range(k,j):
            out[order[p]] = avg
        k = j
    return out


def pearson(a,b):
    ma = statistics.mean(a); mb = statistics.mean(b)
    da = [x-ma for x in a]; db = [x-mb for x in b]
    den = math.sqrt(sum(x*x for x in da)*sum(y*y for y in db))
    return sum(x*y for x,y in zip(da,db))/den if den else float('nan')


def spearman(a,b):
    return pearson(ranks(a), ranks(b))


def sd(xs):
    return statistics.stdev(xs) if len(xs) > 1 else 0.0


def parse_plan(path):
    out = {}
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            groups = [int(x) for x in r['groups'].split(':') if x]
            if len(groups) != 16 or len(set(groups)) != 16:
                raise ValueError(f"{r['name']}: plan must contain 16 unique groups")
            out[r['name']] = groups
    if set(out) != set(TARGET_ORDER):
        raise ValueError('plan.csv must contain victim,o0,o25,o50,o75,o100')
    return out


def load_run_profile(path):
    by_name = defaultdict(lambda:[0.0]*N_GROUPS)
    seen = defaultdict(set)
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            name = r['name']; g = int(r['group']); frac = float(r['frac'])
            if name not in TARGET_ORDER or not 0 <= g < N_GROUPS:
                raise ValueError(f'{path}: invalid name/group')
            if g in seen[name]:
                raise ValueError(f'{path}: duplicate group {name}/{g}')
            seen[name].add(g); by_name[name][g] = frac
    for n in TARGET_ORDER:
        if seen[n] != set(range(N_GROUPS)):
            raise ValueError(f'{path}: incomplete 32-bin profile for {n}')
        by_name[n] = normalize(by_name[n])
    return by_name


def load_targets(path):
    out={}
    with open(path,newline='') as f:
        for r in csv.DictReader(f):
            out[r['name']] = {k:v for k,v in r.items()}
    if set(out) != set(TARGET_ORDER):
        raise ValueError(f'{path}: incomplete targets')
    return out


def find_runs(root):
    runs=[]
    for d in sorted(root.glob('run*')):
        if not d.is_dir():
            continue
        try: rid=int(d.name[3:])
        except ValueError: continue
        p=d/'run_profile.csv'; t=d/'run_targets.csv'
        if p.exists() and t.exists(): runs.append((rid,p,t))
    return runs


def main():
    ap=argparse.ArgumentParser(description='Step2B actual 32-bin weighted-Jaccard gate from existing profile-repeat runs')
    ap.add_argument('result_dir', help='step2b_profile_repeat_YYYYMMDD_HHMMSS directory')
    ap.add_argument('--expected-runs', type=int, default=5)
    ap.add_argument('--min-gap-over-pooled-sd', type=float, default=3.0)
    ap.add_argument('--output-csv', default=None)
    args=ap.parse_args()

    root=Path(args.result_dir)
    if not root.is_dir():
        raise SystemExit(f'[ERROR] result dir not found: {root}')
    plan_path=root/'plan.csv'
    if not plan_path.exists():
        raise SystemExit(f'[ERROR] missing {plan_path}')
    plan=parse_plan(plan_path)
    runs=find_runs(root)
    if len(runs) != args.expected_runs:
        raise SystemExit(f'[ERROR] expected {args.expected_runs} complete run directories, found {len(runs)}')

    rows=[]; obs_by_candidate=defaultdict(list); pred_by_candidate=defaultdict(list)
    all_run_pass=True
    print('JOBS_GATE_HEADER,run,candidate,overlap_pct,J_obs,J_pred,J_obs_minus_pred')
    run_summaries=[]
    for rid,profile_path,target_path in runs:
        prof=load_run_profile(profile_path); tr=load_targets(target_path)
        victim=prof['victim']
        mv=float(tr['victim']['m_i'])
        pred_v=predicted_profile(plan['victim'], mv)
        jobs=[]; jpred=[]
        for c in CANDIDATES:
            mi=float(tr[c]['m_i'])
            jo=weighted_jaccard(victim, prof[c])
            jp=weighted_jaccard(pred_v, predicted_profile(plan[c], mi))
            jobs.append(jo); jpred.append(jp)
            obs_by_candidate[c].append(jo); pred_by_candidate[c].append(jp)
            row={'run':rid,'candidate':c,'overlap_pct':EXPECTED_OVERLAP[c],
                 'J_obs':jo,'J_pred':jp,'J_obs_minus_pred':jo-jp}
            rows.append(row)
            print(f'JOBS_GATE,{rid},{c},{EXPECTED_OVERLAP[c]},{jo:.9f},{jp:.9f},{jo-jp:+.9f}')
        gaps=[jobs[i+1]-jobs[i] for i in range(4)]
        rho=spearman([EXPECTED_OVERLAP[c] for c in CANDIDATES], jobs)
        mono=strict_monotonic(jobs)
        ok=mono and math.isfinite(rho) and abs(rho-1.0) < 1e-12 and min(gaps)>0
        all_run_pass &= ok
        print('JOBS_RUN_SUMMARY,' + ','.join([
            f'run={rid}',f'spearman_rho={rho:.9f}',f'monotonic={"PASS" if mono else "FAIL"}',
            f'min_adjacent_gap={min(gaps):.9f}',
            f'gaps={":".join(f"{x:.9f}" for x in gaps)}',
            f'max_abs_obs_minus_pred={max(abs(a-b) for a,b in zip(jobs,jpred)):.9f}',
            f'status={"PASS" if ok else "FAIL"}']))
        run_summaries.append((rid,rho,min(gaps),ok))

    # Cross-run aggregate and separation diagnostics.
    print('JOBS_AGGREGATE_HEADER,candidate,overlap_pct,J_obs_mean,J_obs_sd,J_obs_cv_pct,J_pred_mean,J_pred_sd,obs_minus_pred_mean')
    for c in CANDIDATES:
        os=obs_by_candidate[c]; ps=pred_by_candidate[c]
        cv=(sd(os)/statistics.mean(os)*100.0) if statistics.mean(os)!=0 else float('nan')
        print(f'JOBS_AGGREGATE,{c},{EXPECTED_OVERLAP[c]},{statistics.mean(os):.9f},{sd(os):.9f},{cv:.6f},{statistics.mean(ps):.9f},{sd(ps):.9f},{statistics.mean([o-p for o,p in zip(os,ps)]):+.9f}')

    print('JOBS_ADJACENT_HEADER,left,right,mean_gap,pooled_sd,gap_over_pooled_sd')
    zvals=[]
    for a,b in zip(CANDIDATES[:-1], CANDIDATES[1:]):
        gaps=[y-x for x,y in zip(obs_by_candidate[a], obs_by_candidate[b])]
        # Pooled between-run SD of the two candidate J_obs distributions.
        sa=sd(obs_by_candidate[a]); sb=sd(obs_by_candidate[b])
        pooled=math.sqrt((sa*sa+sb*sb)/2.0)
        z=statistics.mean(gaps)/pooled if pooled>0 else float('inf')
        zvals.append(z)
        ztxt='inf' if math.isinf(z) else f'{z:.6f}'
        print(f'JOBS_ADJACENT,{a},{b},{statistics.mean(gaps):.9f},{pooled:.9f},{ztxt}')

    # Diagnostic correlation between observed and predicted J across all 15 points.
    obs_all=[float(r['J_obs']) for r in rows]
    pred_all=[float(r['J_pred']) for r in rows]
    corr=pearson(obs_all,pred_all)
    expected_total=args.expected_runs
    passed=sum(1 for _,_,_,ok in run_summaries if ok)
    min_run_gap=min(x[2] for x in run_summaries)
    min_z=min(zvals) if zvals else float('nan')
    separation_ok = (not math.isnan(min_z)) and min_z >= args.min_gap_over_pooled_sd
    hard_ok = all_run_pass and passed==expected_total and separation_ok
    ztxt='inf' if math.isinf(min_z) else f'{min_z:.6f}'
    print('JOBS_FINAL,' + ','.join([
        f'runs={passed}/{expected_total}',
        f'strict_monotonic_runs={sum(1 for x in run_summaries if x[3])}/{expected_total}',
        f'min_run_adjacent_gap={min_run_gap:.9f}',
        f'obs_pred_pearson={corr:.9f}',
        f'min_adjacent_gap_over_pooled_sd={ztxt}',
        f'min_gap_over_pooled_sd_gate={args.min_gap_over_pooled_sd:.6f}',
        f'separation_gate={"PASS" if separation_ok else "FAIL"}',
        f'hard_criterion=all_5_runs_rho1_strict_monotonic_and_aggregate_gap_ge_3pooledsd',
        f'status={"PASS" if hard_ok else "FAIL"}']))
    print('FINAL5_ORACLE_GATE=' + ('PASS' if hard_ok else 'FAIL'))
    print('STEP2B_FINAL5_ORACLE=' + ('PASS' if hard_ok else 'FAIL'))

    out=Path(args.output_csv) if args.output_csv else root/'j_obs_gate.csv'
    with open(out,'w',newline='') as f:
        w=csv.DictWriter(f, fieldnames=list(rows[0].keys())); w.writeheader(); w.writerows(rows)
    print(f'JOBS_CSV={out}')
    raise SystemExit(0 if hard_ok else 3)

if __name__=='__main__':
    main()

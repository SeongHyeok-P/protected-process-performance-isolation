#!/usr/bin/env python3
import tempfile, csv, subprocess, pathlib
ROOT=pathlib.Path(tempfile.mkdtemp(prefix='jobs_selftest_'))
(ROOT/'plan.csv').write_text('kdamond_id,name,groups\n0,victim,'+':'.join(map(str,range(16)))+'\n1,o0,'+':'.join(map(str,range(16,32)))+'\n2,o25,'+':'.join(map(str,list(range(4))+list(range(16,28))))+'\n3,o50,'+':'.join(map(str,list(range(8))+list(range(16,24))))+'\n4,o75,'+':'.join(map(str,list(range(12))+list(range(16,20))))+'\n5,o100,'+':'.join(map(str,range(16)))+'\n')
for run in (1,2,3,4,5):
    d=ROOT/f'run{run}'; d.mkdir()
    names=['victim','o0','o25','o50','o75','o100']
    groups={}
    with open(ROOT/'plan.csv') as f:
        rr=list(csv.DictReader(f))
    for r in rr: groups[r['name']]=set(map(int,r['groups'].split(':')))
    with open(d/'run_profile.csv','w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=['run','name','group','units','frac']); w.writeheader()
        for n in names:
            # 0.85 selected mass, 0.15 nonselected mass: monotonic by set overlap.
            for g in range(32):
                frac=(0.85/16 if g in groups[n] else 0.15/16)
                w.writerow({'run':run,'name':n,'group':g,'units':1,'frac':frac})
    with open(d/'run_targets.csv','w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=['run','name','m_i']); w.writeheader()
        for n in names: w.writerow({'run':run,'name':n,'m_i':0.85})
p=subprocess.run(['python3',str(pathlib.Path(__file__).with_name('analyze_final5_j_obs.py')),str(ROOT),'--expected-runs','5','--min-gap-over-pooled-sd','3.0'],text=True,capture_output=True)
if p.returncode != 0 or 'STEP2B_FINAL5_ORACLE=PASS' not in p.stdout:
    print(p.stdout); print(p.stderr); raise SystemExit(1)
print('FINAL5_JOBS_ANALYZER_SELFTEST_OK')

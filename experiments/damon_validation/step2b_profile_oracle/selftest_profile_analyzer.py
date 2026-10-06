#!/usr/bin/env python3
import csv, subprocess, tempfile
from pathlib import Path

ROOT=Path(__file__).resolve().parent
AN=ROOT/'analyze_profile_repeatability.py'
names=['victim','o0','o25','o50','o75','o100']
sel={n:list(range(16)) for n in names}

with tempfile.TemporaryDirectory() as td:
    d=Path(td)
    with open(d/'plan.csv','w',newline='') as f:
        w=csv.writer(f); w.writerow(['kdamond_id','name','groups'])
        for i,n in enumerate(names): w.writerow([i,n,':'.join(map(str,sel[n]))])
    with open(d/'kmap.csv','w',newline='') as f:
        w=csv.writer(f); w.writerow(['kdamond_id','kdamond_pid','target_pid','name'])
        for i,n in enumerate(names): w.writerow([i,1000+i,2000+i,n])
    for i,n in enumerate(names):
        with open(d/f'{n}.pages.csv','w',newline='') as f:
            w=csv.writer(f); w.writerow(['page_index','va','pfn','group','hot'])
            base=0x10000000+i*0x1000000
            for p in range(64):
                g=p%32; hot=1 if g<16 else 0
                w.writerow([p,hex(base+p*4096),50000+i*100+p,g,hot])
    # 8 one-region complete bursts per emitter, one in each second.
    with open(d/'trace','w') as f:
        for secbin in range(8):
            for i,n in enumerate(names):
                base=0x10000000+i*0x1000000
                sec=100+secbin
                f.write(f'kdamond.{i}-{1000+i} [000] .... {sec}.100000: damon_aggregated: target_id=0 nr_regions=1 {base}-{base+64*4096}: 10 0\n')
    cmd=['python3',str(AN),'run',str(d/'trace'),'--kdamond-map',str(d/'kmap.csv'),'--page-map-dir',str(d),'--plan',str(d/'plan.csv'),
         '--measurement-start-ns',str(100_000_000_000),'--profile-sec','8','--run-id','1','--min-aggs-per-window','1',
         '--output-profile-csv',str(d/'p1.csv'),'--output-target-csv',str(d/'t1.csv'),'--output-window-csv',str(d/'w1.csv')]
    r=subprocess.run(cmd,capture_output=True,text=True)
    if r.returncode!=0 or 'RUN_STRUCTURE_CHECK=PASS' not in r.stdout:
        print(r.stdout); print(r.stderr); raise SystemExit('run selftest failed')
    # duplicate three identical runs for aggregate gate
    for k in (2,3):
        (d/f'p{k}.csv').write_text((d/'p1.csv').read_text().replace(',1,',f',{k},'))
        # safer rewrite run field through csv
        for src,prefix in [('t1.csv','t'),('w1.csv','w')]:
            rows=list(csv.DictReader(open(d/src)))
            out=d/f'{prefix}{k}.csv'
            with open(out,'w',newline='') as f:
                ww=csv.DictWriter(f,fieldnames=rows[0].keys()); ww.writeheader()
                for row in rows: row['run']=str(k); ww.writerow(row)
        rows=list(csv.DictReader(open(d/'p1.csv')))
        with open(d/f'p{k}.csv','w',newline='') as f:
            ww=csv.DictWriter(f,fieldnames=rows[0].keys()); ww.writeheader()
            for row in rows: row['run']=str(k); ww.writerow(row)
    cmd=['python3',str(AN),'aggregate','--profile-csvs',str(d/'p1.csv'),str(d/'p2.csv'),str(d/'p3.csv'),
         '--target-csvs',str(d/'t1.csv'),str(d/'t2.csv'),str(d/'t3.csv'),
         '--window-csvs',str(d/'w1.csv'),str(d/'w2.csv'),str(d/'w3.csv')]
    r=subprocess.run(cmd,capture_output=True,text=True)
    if r.returncode!=0 or 'PROFILE_REPEATABILITY_GATE=PASS' not in r.stdout:
        print(r.stdout); print(r.stderr); raise SystemExit('aggregate selftest failed')
print('PROFILE_ANALYZER_SELFTEST_OK')

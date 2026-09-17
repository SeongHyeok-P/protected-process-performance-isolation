#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

make clean >/dev/null
make -j2 >/dev/null
make test

grep -q 'cfg->sample_us=20000UL' src/damon_multi_observer.c
grep -q 'cfg->aggr_us=400000UL' src/damon_multi_observer.c
grep -q 'cfg->max_regions=1000U' src/damon_multi_observer.c
grep -q 'WNUM("contexts/0/targets/nr_targets",1)' src/damon_multi_observer.c
grep -q 'new_sample->utime_ticks + new_sample->stime_ticks' src/proc_activity.c
if grep -A20 'static int compare_candidate_desc' src/activity_prefilter.c | grep -q 'legacy_total_score'; then
  echo 'FAIL: legacy tie-break still present' >&2
  exit 1
fi

echo 'STATIC_INVARIANTS=PASS'


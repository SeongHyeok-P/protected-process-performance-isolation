# Stage 6-G2: DAMON activity -> recovered DRAM class projection

## Goal

This is the first G2 implementation after Stage 7-A validation.

The question is:

> G1 showed that ordinary allocated physical addresses are spread across nearly all 128 recovered classes.  
> When runtime memory activity is weighted by DAMON, does the recovered-class distribution remain uniform, or does it become skewed?

For this first validation, DAMON is deliberately fixed to:

- `sample_us = 5000`
- `aggr_us = 100000`
- `update_us = 1000000`
- `min_regions = 128`
- `max_regions = 1000`

Dynamic selection of `min_regions` is postponed until after we establish whether G2 produces a useful signal.

## Important interpretation

DAMON does **not** report exact loads/stores and does not identify a 64-byte cache line.

For every DAMON region sample, this implementation:

1. Reads each present 4 KiB base page in the region from `/proc/<pid>/pagemap`.
2. Converts PFN -> physical page base.
3. Computes the recovered class opportunities of all 64-byte line starts inside that page.
4. Gives every page in the DAMON region the region's `nr_accesses`.
5. Splits that page weight among its possible recovered classes according to the number of 64-byte line starts belonging to each class.

Therefore `WEIGHTED` is an **activity-weighted recovered-class estimate**, not a hardware memory-request count.

With the currently recovered seven masks, a 4 KiB page is expected to expose four recovered classes among its 64 line starts. The program prints the actual `page_class_deltas` count at startup.

## Histograms

The final report contains five distributions:

- `WEIGHTED`: primary G2 result. `nr_accesses`-weighted class estimate.
- `HOT`: pages in regions with `nr_accesses >= hot_threshold` (default 10). Diagnostic only.
- `LOW_ACTIVE`: `0 < nr_accesses < hot_threshold`. Diagnostic only.
- `COLD`: pages in regions with `nr_accesses == 0`. Diagnostic only.
- `SPATIAL`: same sampled physical pages without activity weighting. A within-run spatial baseline.

The most important comparison is:

`COMPARE,WEIGHTED,SPATIAL,...`

If `SPATIAL` is close to uniform but `WEIGHTED` is more concentrated/skewed, that supports the G2 hypothesis.

`HOT`/`COLD` are useful diagnostics, but the real G2 ranking should ultimately use the continuous `WEIGHTED` histogram rather than a hard hot/cold threshold.

## Build

From the `src/` directory:

```bash
make -f Makefile.g2 clean
make -f Makefile.g2
make -f Makefile.g2 test
```

Expected unit-test output:

```text
[PASS] G2 class projector unit test
```

The executables are created in `../bin/`.

## Run

The validated DRAM calibration file is expected at:

```text
/run/protected-daemon/dram-map.json
```

Run a workload, obtain its PID, then:

```bash
./run_g2.sh PID 30
```

or directly:

```bash
sudo ./bin/damon_class_observer_cli --duration 30 PID \
  > results/g2.csv
```

A different calibration path can be supplied with:

```bash
sudo ./bin/damon_class_observer_cli \
  --calibration /path/to/dram-map.json \
  --duration 30 PID
```

## What to send back first

After one run:

```bash
grep -E '^(PROJECTOR_STATS|SUMMARY|COMPARE)' results/g2*.csv
```

Also send the mapping/startup header:

```bash
head -20 results/g2*.csv
```

We will first check:

1. `min_regions=128` really applied.
2. `page_class_deltas=4` for the current mapping.
3. PFNs are readable (`pages_zero_pfn` should not dominate).
4. `WEIGHTED` vs `SPATIAL` concentration:
   - coverage
   - top16 share
   - normalized entropy
   - effective classes
   - total variation / weighted Jaccard / cosine similarity

Only after that should we run several real workloads and discuss candidate ranking.

## Caveat for measurement overhead

This first implementation rereads pagemap for every DAMON region sample so the PA mapping is current and no stale PFN cache is assumed. That is deliberately simple and defensible for G2 signal validation, but it adds observer overhead.

If G2 produces a useful class-skew signal, the next engineering step is to reduce this cost using batching/caching/refresh intervals and then remeasure overhead.


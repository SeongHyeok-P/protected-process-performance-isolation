# Stage 7-A: DAMON Observer

Purpose: manually provide one or more PIDs and observe DAMON virtual-address regions and their access activity. This stage intentionally does **not** use candidate_filter, activity_prefilter, ranking, VA->PA mapping, recovered-class mapping, or cpu.max control.

Default monitoring parameters follow the earlier standalone validation:
- sample_us = 5000
- aggr_us = 100000
- update_us = 1000000
- nr_regions = 10..1000

With 5 ms sampling and 100 ms aggregation, `nr_accesses` is approximately in the range 0..20 for each aggregation window. It is a DAMON sampled activity metric, not an exact load/store or DRAM-request count.

## Build

```bash
make
make test
```

## Run

```bash
sudo ./damon_observer_cli --duration 10 <PID>
```

Multiple targets:

```bash
sudo ./damon_observer_cli --duration 10 <PROTECTED_PID> <CANDIDATE_PID>
```

Output:

```text
TYPE,elapsed_ms,target_id,pid,nr_regions,start,end,size_bytes,nr_accesses,age
REGION,...
```

This implementation uses `/sys/kernel/mm/damon/admin` and the `damon_aggregated` tracepoint under `/sys/kernel/tracing`.

Important: start currently resets `kdamonds/nr_kdamonds` to 0 then 1, matching the standalone test procedure. Do not run it concurrently with another DAMON user that owns the same sysfs admin interface.

## Next use in 6-G2

After this observer is validated, 6-G2 can consume each region `(start, end, nr_accesses)` and add:

```text
hot VA region -> pagemap -> PA -> recovered class -> activity-weighted histogram
```

That conversion is intentionally not part of 7-A yet.
# Stage 7-A controlled DAMON validation workload

Purpose: validate the DAMON observer against a process whose access pattern is known in advance.

Default layout (single process):

- HOT: 8 MiB. Continuously sweeps one byte from every 4 KiB page.
- WARM: 8 MiB. Sweeps one byte from every 4 KiB page every 50 ms.
- COLD: 240 MiB. Faults each page once during setup and does not touch it during the measured phase.
- A PROT_NONE guard page separates HOT/WARM/COLD so the address ranges are distinct VMAs.
- MADV_NOHUGEPAGE is requested for each data region.

With the Stage 7-A observer defaults (`sample_us=5000`, `aggr_us=100000`), the theoretical maximum `nr_accesses` per aggregation is 20. Expected qualitative order is:

    HOT  >> WARM >> COLD

Do not expect an exact 20 / 2 / 0 on every DAMON region because DAMON is region-based sampling and adaptively splits/merges regions. The validation criterion is that the printed HOT address range is consistently much hotter than WARM, and WARM is hotter than COLD after the initial setup effect disappears.

## Install into the current project

From `~/cgroup_research/experiments/stage7A_damon_observer`:

```bash
cp <downloaded>/src/damon_access_pattern_workload.c src/
cp <downloaded>/src/Makefile.workload src/
cp <downloaded>/run_validation.sh ./

cd src
make -f Makefile.workload
cd ..
```

The binary is created as:

```text
bin/damon_access_pattern_workload
```

## Manual validation

Terminal 1:

```bash
./bin/damon_access_pattern_workload --duration 60
```

It prints exact HOT/WARM/COLD VA ranges and `READY,pid=...`.

Terminal 2:

```bash
sudo ./bin/damon_observer_cli --duration 15 <PID> \
  > results/stage7A_damon_validation.csv
```

Compare DAMON region addresses with the ranges printed by the workload.

## One-command helper

If `run_validation.sh` is in the project root:

```bash
./run_validation.sh 15
```

It starts the workload, extracts its PID, runs the existing DAMON observer, and saves both logs under `results/`.

## Useful variants

Make WARM colder:

```bash
./bin/damon_access_pattern_workload --warm-period-ms 200
```

Smaller quick test:

```bash
./bin/damon_access_pattern_workload \
  --hot-mib 4 --warm-mib 4 --cold-mib 56 --duration 30
```

This workload validates only Stage 7-A VA-level observation. It intentionally does not perform VA->PA conversion or recovered-class classification; those belong to 6-G2 after the observer is trusted.


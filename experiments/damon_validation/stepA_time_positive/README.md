# Step A3/A4/A5 + Step P validation

This package keeps the recovered-group mapping and WEIGHTED / WEIGHTED_NORM formulas unchanged.

It adds:

1. a clean kernel ftrace `damon_aggregated` session: stale trace data is cleared, the trace clock is temporarily forced to `mono`, timestamps are parsed as `trace_ts_ns`, and the previous clock is restored on shutdown,
2. complete-aggregation buffering after dropping the initial partial burst,
3. real-time 1 s windows based on kernel trace timestamps rather than `10 aggregations == 1 s`,
4. a 1 s warm-up drop before measurement windows,
5. observer parse counters and per-window HOT/LOW/COLD diagnostics,
6. callback timing diagnostics using relative elapsed clocks so kernel aggregation cadence can be distinguished from user-space projection backlog without assuming ftrace and CLOCK_MONOTONIC share an epoch,
7. a positive-control workload that keeps the entire pool faulted-in and RW in one unchanged VMA while touching only pages belonging to selected recovered groups,
8. 4 / 8 / 16 selected-group sweep with a constant total number of hot pages.

## Build

```bash
cd stepA_time_positive_v2
./build.sh
```

Expected end:

```text
BUILD_OK
PASS trace_timestamp_ns=116363596202000
PASS warmup_and_trace_time_window_logic
```

## 1. Re-run the uniform negative control with real-time windows

```bash
sudo -v
./run_time_negative.sh
```

Then show:

```bash
DIR=$(ls -dt stepA_time_* | head -1)
cat "$DIR/time_summary.txt"
grep -E 'AGG_SYNC|AGG_COMPLETE|AGG_WARMUP_DROP|WINDOW_TIME_META|TIME_BOUNDARY_SUMMARY|OBSERVER_PARSE_STATS' "$DIR/observer.log" | head -80
cat "$DIR/observer.err"
cat "$DIR/control.txt"
```

Interpretation:
- `agg_delta_us` is now computed from the kernel trace timestamps, not callback time.
- `callback_minus_trace_rel_us` / `callback_span_us` show user-space backlog/processing behavior. The lag is computed from elapsed time since AGG_SYNC, not by subtracting unrelated absolute clock epochs.
- every reported full `WINDOW_TIME_META` has exactly a 1000 ms trace-time bin, while the number of aggregations per bin is allowed to vary.
- the first 1000 ms after synchronization is dropped as warm-up.
- `parse_fail=0` is expected.

## 2. Group-selective positive control

After the negative run is sane:

```bash
sudo ./run_positive_sweep.sh
```

This runs three conditions while keeping total hot pages fixed (`HOT_PAGES_TOTAL=3072` by default):

- 4 groups: `0,8,16,24` -> 768 hot pages/group
- 8 groups: `0,4,8,12,16,20,24,28` -> 384 hot pages/group
- 16 groups: even dense groups -> 192 hot pages/group

The whole 128 MiB pool is faulted-in before monitoring and remains one RW VMA. Non-selected pages are not `PROT_NONE`; they remain present and cold. The workload records the exact full-pool `POOL_GROUP` distribution and verifies every pool PFN at the end (`PFN_VERIFY_ALL`). Therefore SPATIAL has an explicit physical-pool ground truth while activity is deliberately selective.

Show:

```bash
ROOT=$(ls -dt stepP_sweep_* | head -1)
for D in "$ROOT"/g4 "$ROOT"/g8 "$ROOT"/g16; do
  echo "===== $D ====="
  cat "$D/control.txt"
  cat "$D/positive_summary.txt"
  cat "$D/time_summary.txt"
  cat "$D/observer.err"
done
```

Do not choose WEIGHTED or WEIGHTED_NORM from the uniform negative test.  Compare both on the positive sweep.  The useful signal is enrichment of selected-group mass above both the exact pool baseline and observer SPATIAL, similarity to the known selected-page distribution, SPATIAL similarity to the exact `POOL_GROUP` profile, and top-k recovery across 4/8/16 groups.


## Acceptance before 6-PID

Do not move to 6-PID until the negative-control run shows all of the following:

- `TIME_STRUCTURE_CHECK=PASS`,
- `parse_fail=0`,
- synchronized complete aggregations with no boundary error on stderr,
- full measured windows have exactly the configured 1000 ms trace width,
- the first measured window begins after the full 1000 ms warm-up,
- `agg_trace_delta_us` is stable enough to characterize the kernel DAMON cadence, and
- `callback_minus_trace_rel_us` does not show unbounded growth. A growing value means user-space projection/callback handling is falling behind the kernel trace stream.

Only after that run the 4/8/16 positive-control sweep. Positive-control semantic PASS is not claimed by this package itself; it must be decided from the produced logs.

Note: this validation tool intentionally treats the global tracefs buffer/clock as an exclusive experiment resource while it runs. Do not run another ftrace consumer concurrently.

## One-command gated sequence

To enforce the intended order automatically:

```bash
sudo -v
./run_time_then_positive.sh
```

`run_time_negative.sh` exits nonzero unless the observer/workload return codes are zero and `TIME_STRUCTURE_CHECK=PASS`. Only then does the wrapper start the 4/8/16 sweep. Each positive condition also requires time-structure PASS and `PFN_VERIFY_ALL,moved=0,unreadable=0`.

## v4 packaging correction
This verified bundle is intentionally self-contained.  Do not copy only
`damon_class_observer_cli_time.c` into an older Step-A directory.  The CLI,
`class_projector.[ch]`, and `damon_observer.[ch]` are one API set.
`./build.sh` first runs `verify_bundle.sh` and refuses mixed-generation files.

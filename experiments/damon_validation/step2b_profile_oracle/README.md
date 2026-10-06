# Step 2B — Final 5-run oracle holdout

This is the final semantic holdout for the validated Step 2B topology.  It does **not** reuse the previous 3 development/gate runs.

## Frozen experiment

- 6 kdamonds
- 1 context / 1 target per kdamond
- `min_regions=128`, `max_regions=2000` per kdamond
- sample / aggregation / update = 5 ms / 100 ms / 1 s
- pool = 160 MiB per workload
- hot/selected pages = 16384 (64 MiB)
- 16 selected recovered-class groups per workload
- estimator = full 32-bin `WEIGHTED_NORM` (`alpha=0`)
- 4 s warm-up
- 8 s measured profile interval
- **5 new independent runs**

The runner allocates new workload pools/processes on every run.  The old 3-run development dataset is not included in final acceptance statistics.

## Calibration freeze

At experiment start the runner:

1. reads `/run/protected-daemon/dram-map.json` with sudo,
2. copies it to `dram-map.frozen.json` in the result directory,
3. records its `mask_set_id` and SHA-256,
4. verifies before every run and after the five runs that the live calibration file has not changed.

The script intentionally does not hard-code a mask-set ID.  It freezes the runtime calibration that is actually current when the final holdout begins.

## Hard acceptance criteria — declared before the 5 runs

For each run, using the **actual observed 32-bin WN profiles**:

`J_obs(victim, candidate) = sum_g min(P_v[g], P_i[g]) / sum_g max(P_v[g], P_i[g])`

Every one of the five new runs must satisfy:

- `J_obs(o0) < J_obs(o25) < J_obs(o50) < J_obs(o75) < J_obs(o100)`
- Spearman rho against nominal overlap `[0,25,50,75,100]` = exactly `1.0`
- every adjacent gap is positive

Across the five runs, each adjacent candidate pair is summarized by mean J gap and pooled between-run SD.  The final aggregate criterion is:

- minimum `mean_gap / pooled_SD >= 3.0`

Therefore the final gate passes only if **all five runs preserve the exact ordering AND the weakest aggregate adjacent separation is at least 3 pooled SD**.

`J_pred` from the two-level `m_i` model is still printed as an analytical diagnostic, but it is not the estimator used for the hard semantic decision.

## No replacement rule

A semantic failure is a real final-holdout failure.  Do **not** rerun only the failed semantic run and replace it.

If the runner aborts because of an infrastructure invalidity (build failure, malformed/unknown trace identity, PFN verification failure, calibration changed during the experiment, etc.), fix the infrastructure issue and start a fresh complete 5-run holdout.  Do not cherry-pick the valid-looking partial runs.

## Run

Copy/unzip these files into the same StepB working directory where the previous profile gate ran.

```bash
chmod +x build_final5_oracle.sh run_final5_oracle.sh
sudo -v
./run_final5_oracle.sh | tee final5_oracle.console.log
```

Build should first print:

```text
PROFILE_ANALYZER_SELFTEST_OK
FINAL5_JOBS_ANALYZER_SELFTEST_OK
BUILD_FINAL5_ORACLE_OK
```

At experiment start it prints the frozen calibration identity:

```text
FROZEN_CALIBRATION,mask_set_id=...,sha256=...
FINAL5_PLAN,runs=5,...
```

Final successful tail:

```text
JOBS_FINAL,runs=5/5,...,separation_gate=PASS,...,status=PASS
FINAL5_ORACLE_GATE=PASS
STEP2B_FINAL5_ORACLE=PASS
RESULT_DIR=step2b_final5_oracle_...
```

Important result files:

- `FROZEN_INPUTS.txt`
- `dram-map.frozen.json`
- `source_sha256.txt`
- `run1/ ... run5/` raw trace/page maps/run profiles
- `final5_j_obs.csv`
- `final5_oracle.txt`

## Interpretation

A PASS supports the following narrow claim: under the frozen per-target DAMON validation topology, the actual full 32-bin WN recovered-group profiles preserve the controlled 0/25/50/75/100 overlap ordering across five independent physical allocations with at least 3-pooled-SD aggregate adjacent separation.

It does not yet establish production overhead or prove the UC probe adds predictive value.  Those are later stages.

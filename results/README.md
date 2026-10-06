# Curated result tables

This directory intentionally contains compact, paper-facing summaries rather than raw run directories.

- `controlled_overlap.csv`: controlled LOW/HIGH activity overlap sweep used for the latency-contribution result.
- `partition_robustness.csv`: complementary 16/16 recovered-group partition checks.
- `synthetic_candidate_ranking.csv`: controlled multi-candidate intervention recovery and the accepted `A × J` ranking.
- `natural_profile_summary.csv`: 32-group DAMON `WEIGHTED_NORM` flatness/repeatability summary.
- `natural_interworkload_jaccard.csv`: between-workload weighted-Jaccard comparisons.
- `pmu_raw128_summary.csv`: PMU physical-address sample projection to 128 raw recovered classes.
- `natural_fallback_5block_*`: fixed-placement intervention ground truth and predictor comparison.
- `placement_rotation_*`: 3 CPU placements × 4 blocks final natural-workload validation.

Raw traces, per-process stdout/stderr, temporary cgroup logs, perf logs, calibration scratch files, and repeated exploratory sweeps are deliberately not tracked.

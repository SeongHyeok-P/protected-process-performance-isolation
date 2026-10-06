# Controlled Step-C experiments

논문의 통제 환경 실험에 필요한 핵심 workload/runner만 보존합니다.

- `stepc_activity_overlap_workload.c`: recovered-group 집합과 pacing을 제어하는 workload.
- `run_stepc_c0_pacing.sh`: LOW/HIGH pacing calibration용.
- `run_stepc_c1_groundtruth.sh`: DAMON을 끈 상태에서 activity/overlap 조건의 victim harm을 측정.
- `run_stepc_c2b_actionable_v2.sh`: 5개 후보를 동시에 실행하고 동일한 `cpu.max=20000 100000`으로 후보를 하나씩 제한하여 actionable recovery를 측정.
- `calibration.*`, `dram_mapping.*`: workload가 `/run/protected-daemon/dram-map.json`을 읽기 위한 최소 mapping support.

빌드:

```bash
./build_stepc_c0.sh
```

이 폴더는 실험 소스를 보존하기 위한 것이며, raw run directory는 Git에서 제외합니다. 논문에 사용한 최종 요약값은 repository root의 `results/`에 따로 보존합니다.

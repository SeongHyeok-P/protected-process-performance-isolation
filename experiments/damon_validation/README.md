# DAMON measurement validation

논문 본 실험 전에 사용한 핵심 측정 경로 검증 소스만 보존합니다.

- `stepA_time_positive/`: 128 raw class -> 32 page-observable group projection, trace-time windowing, `WEIGHTED_NORM` positive/negative validation.
- `step2b_profile_oracle/`: group-aware oracle workload를 이용한 profile repeatability / observed J validation.

여러 parameter sweep의 raw output과 이후 폐기된 predictor 실험은 저장하지 않았습니다.

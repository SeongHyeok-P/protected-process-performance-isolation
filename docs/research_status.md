# Research status snapshot

## Problem

보호 프로세스와 동시에 실행되는 여러 후보 중 하나를 제한해야 할 때, recovered DRAM spatial information이 실제 제한 가치(actionable recovery)의 순위를 보존하는지 평가합니다.

## Current pipeline

1. DRAM mapping calibration / recovered classes.
2. 4 KiB page 관측에서 구분 가능한 32 recovered groups로 canonicalization.
3. DAMON `nr_accesses` 기반 `WEIGHTED_NORM` group activity profile.
4. Victim-candidate normalized weighted Jaccard `J`.
5. `proc_activity`의 bounded cheap activity `A`.
6. candidate heuristic `S=A×J`.
7. cgroup v2 `cpu.max` intervention으로 실제 victim recovery 확인.

## Current interpretation

Controlled workload에서는 overlap이 latency harm에 기여하며 spatial/activity signal이 구별될 때 `A×J`도 유용한 ranking을 만들 수 있습니다. 그러나 평가한 natural GAPBS workload에서는 recovered-group profile이 거의 포화되고 bounded `A`도 포화되어 passive fusion이 실제 intervention-value ordering을 안정적으로 복원하지 못했습니다.

따라서 현재 연구는 “mapping을 복원하면 간섭 프로세스를 고를 수 있다”는 가정보다, **mapping validity와 control-oriented spatial discriminability를 분리해서 검증해야 한다**는 limitation/validation 방향에 초점을 둡니다.

## Not yet claimed

- 완성된 online adaptive policy
- 모든 CPU/DRAM architecture에 대한 일반화
- `cpu.max`가 memory bandwidth를 일정 비율로 직접 줄인다는 가정
- PMU sample이 전체 DRAM traffic ground truth라는 주장
- `A×J`가 모든 baseline보다 우월하다는 주장

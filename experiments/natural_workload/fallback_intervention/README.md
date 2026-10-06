# Natural-workload fallback intervention experiment

이 묶음은 논문의 마지막 추가 검증을 한 번의 통합 실행으로 수행합니다.

## 연구 질문

자연 workload에서 recovered-group 공간 분포가 후보 프로세스를 안정적으로 구별하지 못할 때,

1. 현재 daemon의 **활동도 점수(activity)** 만으로 실제 제한 대상을 고를 수 있는지,
2. **recovered-group 중첩(weighted Jaccard)** 만으로 고를 수 있는지,
3. 기존 **activity × overlap** 결합 점수가 더 나은지

를 **실제 cgroup CPU quota 개입 후 중요 태스크 latency 회복량**과 비교합니다.

`activity 단독 fallback`은 이 실험 전에는 결론이 아니라 **검증할 가설**입니다.

## 파일

- `natural_victim_probe.c`: 64 MiB(기본값) random dependent pointer-chasing 중요 태스크. 같은 PID/물리 페이지를 유지하며 1초 단위 latency를 계속 출력합니다.
- `natural_score_probe.c`: 현재 protected-daemon v2의 `proc_activity`와 Step C DAMON 경로를 재사용합니다. 6 s warmup 후 1 s window에서 activity와 overlap을 함께 관측합니다.
- `run_natural_fallback_intervention.sh`: block, CPU pinning, cgroup quota, score 관측, pre/post latency, `cpu.stat`, optional PMU를 자동화합니다.
- `analyze_natural_fallback.py`: 실제 제한 가치 순위와 activity / overlap / fused 순위를 비교합니다.
- `Makefile`

## 기본 경로

```text
protected-daemon v2:
<repo>/protected-daemon/v2

mapping:
/run/protected-daemon/dram-map.json

graph:
<repo>/datasets/kron_g23.sg

GAPBS:
<repo>/benchmark/gapbs/{bfs,cc,pr}
```

다르면 환경변수로 덮어쓸 수 있습니다.

## 실행

```bash
make
sudo ./run_natural_fallback_intervention.sh
```

helper가 없으면 실행 스크립트가 자동 빌드도 시도하므로 아래 한 줄만으로도 가능합니다.

```bash
sudo ./run_natural_fallback_intervention.sh
```

먼저 로직만 확인하려면:

```bash
sudo \
  BLOCKS=1 \
  GAPBS_TRIALS=100 \
  LATENCY_EPOCHS=2 \
  MEASURE_TRAFFIC=0 \
  ./run_natural_fallback_intervention.sh
```

정식 결과는 기본 `BLOCKS=5`, `GAPBS_TRIALS=1000`으로 다시 수행하세요.

## CPU 배치

기본값:

```text
CPU 0          OS/misc로 남김
CPU 2,4        BFS
CPU 6,8        CC
CPU 10,12      PageRank
CPU 14         중요 태스크
```

GAPBS는 `OMP_NUM_THREADS=2`입니다.

## 한 block의 흐름

한 block 안에서는 중요 태스크와 세 GAPBS 프로세스를 **한 번만 띄우고 유지**합니다. 세 intervention 사이에 PID와 물리 페이지가 새로 할당되는 교란을 줄이기 위한 설계입니다.

```text
중요 태스크 시작
  ↓
victim-only BASE
  ↓
BFS + CC + PageRank 시작
  ↓
workload warmup
  ↓
DAMON ON
  ├─ Step C warmup 6 s
  ├─ activity t0
  ├─ 1 s WEIGHTED_NORM profile window
  ├─ activity t1
  └─ A, J, A×J 계산
  ↓
DAMON 완전히 OFF
  ↓
settle
  ↓
UNCTRL / BFS 제한 / CC 제한 / PageRank 제한
(블록마다 순서를 회전)
  ↓
각 조건:
  quota 해제 → pre latency
  → 지정 후보만 quota (UNCTRL은 제한 없음)
  → settle → post latency → quota 해제
```

`UNCTRL`도 같은 시간만 기다리며 pre/post를 측정하므로 block 내부의 자연 drift를 추정합니다.

후보 `i`의 보정 회복량:

```text
adjusted_recovery_i
  = (pre_i - post_i)
    - (pre_UNCTRL - post_UNCTRL)
```

이 값을 주 ground truth로 사용합니다.

## CPU quota

기본:

```text
cpu.max = 20000 100000
```

기존 통제 candidate intervention과 같은 고정 개입 수준을 유지하기 위한 값입니다. 이는 **CPU time 제한**이지 memory bandwidth 20%가 아닙니다.

각 GAPBS 후보는 두 CPU에 pinning되므로, 두 CPU capacity의 20%인 0.4 CPU를 별도로 시험하고 싶다면:

```bash
sudo THROTTLE_CPU_MAX="40000 100000" ./run_natural_fallback_intervention.sh
```

로 실행할 수 있지만, 제출용 주 실험에서는 중간에 quota를 바꾸지 마세요.

## 비교하는 세 prediction

### 1. 활동도 단독

현재 Step C activity-prefilter 정의를 그대로 사용합니다.

```text
A_i = max(cpu_score, fault_score, rss_score)
```

동시에 다음 원시 진단값도 저장합니다.

- `cpu_ratio`
- `faults_per_sec`
- `rss_mib`
- `cpu_score`, `fault_score`, `rss_score`
- 과거 `total_score` (진단용만)

세 workload가 모두 activity=1.0으로 포화되면 그것 자체가 중요한 결과입니다. 이 경우 현재 구현의 activity-only fallback은 순위를 만들지 못합니다.

### 2. recovered-group 중첩

현재 Step C 경로 그대로입니다.

```text
DAMON region activity
→ VA→PFN/PA
→ 32 page-observable recovered groups
→ WEIGHTED_NORM
→ normalized weighted Jaccard J_i
```

### 3. 결합 점수

```text
S_i = A_i × J_i
```

## 실제 제한 가치

각 intervention의 paired 변화:

```text
raw_recovery_i = pre_i - post_i
```

에서 같은 block의 UNCTRL drift를 빼 `adjusted_recovery_i`를 만듭니다.

질문은 **“같은 CPU quota 하나를 적용해야 한다면 어느 후보를 제한했을 때 중요 태스크가 가장 많이 회복되는가?”**입니다.

## traffic과 cpu.stat

항상 후보 cgroup의:

- `usage_usec`
- `nr_throttled`
- `throttled_usec`

를 기록합니다.

가능하면 PMU event:

```text
cpu_core/mem_load_l3_miss_retired.local_dram/pp
```

를 후보의 현재 thread들에 붙여 제한 전/후에 기록합니다. 이 PMU 값은 **보조 진단**입니다. `recovery / removed traffic`을 주 endpoint로 바꾸지 않습니다.

PMU가 불안정하면:

```bash
sudo MEASURE_TRAFFIC=0 ./run_natural_fallback_intervention.sh
```

로 끄세요.

## 출력

`results/natural_fallback_<timestamp>/` 아래에:

```text
metadata.txt
dram-map.json
blocks.csv
scores.csv
interventions.csv
interventions_with_adjusted.csv
block_rankings.csv
candidate_summary.csv
method_summary.csv
summary.txt
raw/...
```

가 생성됩니다.

우선 확인:

```bash
cat results/natural_fallback_*/summary.txt
column -s, -t results/natural_fallback_*/candidate_summary.csv
column -s, -t results/natural_fallback_*/method_summary.csv
column -s, -t results/natural_fallback_*/block_rankings.csv
```

## 분석 지표

후보가 3개이므로 Top-1만 보고 결론내리지 않습니다.

- unique Top-1 정확 block 수
- top-set이 실제 Top-1을 포함한 block 수
- 3 pair × 5 block = 최대 15 pair의 순서 일치
- prediction tie 수
- block별 Spearman
- 예측 순위가 block마다 얼마나 뒤집히는지
- 실제 recovery 순위의 안정성

을 함께 봅니다.

## 결과 해석

### A. activity 단독이 안정적으로 맞음

```text
공간 분포 차이가 반복 변동보다 충분히 큼
  → activity × overlap

공간 분포가 포화되어 구별 어려움
  → activity 단독
```

이라는 adaptive fallback을 주장할 실험 근거가 생깁니다.

### B. activity 단독도 실패/포화

activity fallback을 억지로 주장하지 않습니다.

```text
공간 정보도 불충분
+ activity도 실제 제한 가치와 안정적으로 연결되지 않음
  → passive score만으로 제한 대상을 정하지 않음
  → 짧은 실제 개입 후 중요 태스크 반응으로 식별
```

으로 갑니다.

### C. activity×overlap이 자연 workload에서도 잘 맞음

공간 histogram이 포화되어도 activity와 결합할 때 제어에 유용한 잔여 정보가 남는다는 positive result가 됩니다.

## GAPBS CLI 주의

스크립트는 다음 형식을 가정합니다.

```bash
bfs -f kron_g23.sg -n 1000
cc  -f kron_g23.sg -n 1000
pr  -f kron_g23.sg -n 1000
```

로컬 GAPBS build의 CLI가 다르면 `run_natural_fallback_intervention.sh`의 `start_one_gapbs()` 한 줄을 기존 `run_real_profile_survey.sh`와 같게 맞추면 됩니다. workload가 즉시 종료하면 script가 fail-fast하고 raw log 위치를 출력합니다.

## 현재 v2 사용 주의

반드시 현재 Step C 통합본에 링크해야 합니다. 과거 `proc_activity_compute_delta()`의 잘못된 `stime` 계산 사본이 감지되면 스크립트는 시작 전에 중단합니다.

## 논문에서의 역할

이 실험은 기존 natural-workload profile saturation을 반복 확인하기 위한 실험이 아닙니다.

> 자연 workload에서 recovered-group 공간 정보가 후보를 안정적으로 구별하지 못하는 상황에서, 실제 제한 대상 선택을 무엇으로 해야 하는가?

를 검증하는 **해결 방안 보강 실험**입니다.

## CPU-placement rotation

`run_placement_rotation.sh`는 위 base experiment를 3개의 cyclic CPU placement에서 각 4 block씩 실행하여 총 12 block의 ranking 안정성을 확인합니다. 논문에 사용한 compact summary는 repository root의 `results/placement_rotation_*.csv`에 보존합니다.

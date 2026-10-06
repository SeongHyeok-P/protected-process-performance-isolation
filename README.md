# 보호 프로세스의 성능 격리를 위한 메모리 간섭 탐지 및 완화 기법 연구

**Memory Interference Detection and Mitigation for Protected-Process Performance Isolation**

이 저장소는 Linux에서 지연 민감한 보호 프로세스의 성능 저하를 유발하는 메모리 간섭 후보를 관측하고, 필요 시 cgroup v2로 제한하는 연구 프로토타입과 핵심 검증 코드를 정리한 것입니다.

현재 연구의 중심 질문은 **복원된 DRAM 주소 매핑과 공간 중첩 정보가 실제로 제한할 프로세스의 우선순위를 안정적으로 보존하는가**입니다. DAMON `nr_accesses`를 recovered-group별 활동 분포로 투영해 weighted Jaccard `J`를 계산하고, 별도의 process-level activity score `A`와 결합한 `S=A×J`를 실제 동일 quota 개입 후 victim latency recovery와 비교합니다.

## 현재까지의 핵심 결과

- 통제 환경에서는 recovered-group 공유율이 증가할수록 victim 추가 지연이 증가했습니다. HIGH 조건의 0%→100% 차이는 약 3.573 ns였습니다.
- 공간/활동 신호가 의도적으로 구별되는 synthetic multi-candidate 조건에서 `A×J`는 실제 제한 가치 Top-1을 맞혔고, Spearman 0.90, pairwise 9/10을 기록했습니다.
- 자연 workload(BFS, CC, PageRank)에서는 32-group profile이 거의 평탄해졌고, raw 128-class PMU cross-check에서도 유사한 포화를 관찰했습니다.
- 자연 workload에서 현재 bounded activity score `A`도 포화되어 `S=A×J`가 사실상 `J`와 같은 순위를 만들었습니다. CPU placement를 회전한 12 block에서 `J`/`A×J`의 Top-1은 7/12, pairwise ordering은 23/36이었습니다.
- 따라서 **mapping validity, overlap의 latency contribution, control-oriented candidate discriminability는 서로 다른 조건**으로 다뤄야 한다는 방향으로 연구를 정리하고 있습니다.

정량 결과는 `results/`의 compact CSV에 보존되어 있습니다. 원시 로그와 반복 parameter sweep은 의도적으로 제외했습니다.

## 저장소 구조

```text
protected-process-performance-isolation/
├── protected-daemon/v2/        # eBPF + DAMON + activity + cgroup 통합 daemon
├── experiments/
│   ├── damon_validation/       # 32-group/WEIGHTED_NORM 측정 경로 검증
│   ├── controlled/stepc/       # 통제 activity/overlap 및 actionable intervention
│   └── natural_workload/       # GAPBS natural workload + 12-block CPU rotation
├── results/                    # 논문에 사용한 핵심 요약 데이터만
├── docs/paper_current.pdf      # 현재 3-page 논문
├── config/                     # 보호 프로세스 설정 예시
├── datasets/                   # 대형 graph는 별도 준비
├── benchmark/gapbs/            # GAPBS는 별도 준비
├── Knock-Knock/                # DRAM mapping dependency는 별도 준비
└── libbpf-bootstrap/           # BPF build dependency는 별도 준비
```

## 측정 신호

DAMON 쪽 spatial signal은 다음 흐름입니다.

```text
DAMON nr_accesses
  -> recovered-group activity H
  -> normalized profile P
  -> weighted Jaccard J
```

후보 전체 activity는 별도의 cheap process-level signal입니다.

```text
CPU usage / page faults / RSS
  -> bounded activity A
```

후보 ranking heuristic은 다음과 같습니다.

```text
S = A × J
```

`S`는 물리적 latency 모델이나 ns 예측식이 아니라 후보 우선순위를 위한 heuristic입니다.

## 빠른 시작

### 1. daemon core/test

```bash
cd protected-daemon/v2
make clean
make -j libstepc_integration.a
make test
```

실제 eBPF daemon까지 빌드하려면 repository root에 `libbpf-bootstrap/`을 준비하고 clang/bpftool/libelf/zlib/BTF 환경을 갖춰야 합니다.

### 2. 통제 workload

```bash
cd experiments/controlled/stepc
./build_stepc_c0.sh
```

실제 실험은 root/DAMON/cgroup v2/pagemap 접근이 필요합니다. `/run/protected-daemon/dram-map.json`이 먼저 준비되어 있어야 합니다.

### 3. natural workload

`datasets/kron_g23.sg`와 `benchmark/gapbs/{bfs,cc,pr}`를 준비한 뒤:

```bash
cd experiments/natural_workload/fallback_intervention
make
sudo ./run_natural_fallback_intervention.sh
sudo ./run_placement_rotation.sh
```

## 현재 범위와 주의점

이 저장소는 현재 연구 시점의 **prototype + reproducibility snapshot**입니다. 제한적 intervention을 이용한 최종 online selection policy, intervention duration/repetition 최적화, 다중 플랫폼 일반화는 아직 완료된 결과가 아닙니다. 또한 PMU raw-class 관측은 sampled local-DRAM retired loads 중심이므로 전체 DRAM request의 ground truth로 해석하지 않습니다.

실험 데이터의 포함/제외 기준과 연구 중 점수식 변경 이력은 `DATA_PROVENANCE.md`를 참고하십시오. 기존 `cgroup_research` GitHub 저장소를 이 구조로 교체하는 절차는 `MIGRATION_FROM_CGROUP_RESEARCH.md`에 정리했습니다.

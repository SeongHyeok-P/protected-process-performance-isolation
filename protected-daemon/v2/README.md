# protected-daemon Step C 통합 묶음

## 1. 이 묶음이 동결하는 것

이 묶음은 Step C에서 실험으로 확인한 **측정 경로**를 protected-daemon용 코드로 옮긴 것입니다.

- recovered raw classes: 128
- page-observable groups: 32
- profile estimator: `WEIGHTED_NORM`
- DAMON topology: **target 1개당 independent kdamond 1개**
- `sample_us = 20000`
- `aggr_us = 400000`
- `update_us = 1000000`
- `nr_regions.min = 128`
- `nr_regions.max = 1000`
- logical runtime profile window: 1 s
- aggregation burst 검증: emitter(kdamond)별로 조립하며 `line_count == nr_regions`인 complete burst만 사용

`bank_profile.c/h`도 128 raw class를 그대로 비교하지 않고 **32 page-observable group으로 canonicalize**한 버전입니다. 단, `bank_profile`은 static/spatial 진단 경로이고, 최종 runtime overlap은 DAMON `WEIGHTED_NORM` weighted Jaccard를 사용합니다.

## 2. 기존 코드에서 유지하는 것

아래 기능은 새로 만들 필요가 없으므로 기존 protected-daemon 설계를 유지합니다.

- eBPF lifecycle discovery
- `process.c/h`
- `candidate_filter.c/h`
- `proc_activity` 기반 cheap observation
- `activity_prefilter` Top-K1
- `cgroup.c/h`
- multi-candidate `throttle_controller`

이 묶음의 새 연결부는 다음입니다.

```text
candidate_filter
  -> proc_activity
  -> activity_prefilter Top-K1
  -> [NEW] independent-kdamond DAMON observer
  -> [NEW] 32-group WEIGHTED_NORM profile
  -> [NEW] weighted-Jaccard refinement
  -> interference_candidate[]
  -> existing throttle_controller
```

## 3. 중요한 비동결 항목

**Step C가 동결한 것은 measurement path이지 최종 policy score 공식이 아닙니다.**

`stepc_damon_detector`는 기존 controller와 연결하기 위해 두 score mode를 제공합니다.

- `STEPC_SCORE_ACTIVITY_X_OVERLAP`: 기존 detector와 호환되는 `activity * J` 형태
- `STEPC_SCORE_OVERLAP_ONLY`: J만 사용

기본값은 기존 코드와의 호환 때문에 `ACTIVITY_X_OVERLAP`이지만, 이 식과 `0.30/0.20` threshold를 논문상 최종 정책으로 확정했다는 의미는 아닙니다. 첫 end-to-end 실험은 throttle controller를 `dry_run=true`로 시작하는 것을 권장합니다.

## 4. 기존 코드에서 수정한 버그/불일치

### proc_activity

라이브러리에 남은 과거 사본의

```c
new_sample->utime_ticks + old_sample->stime_ticks
```

를 사용하지 않습니다. 이 묶음은

```c
new_sample->utime_ticks + new_sample->stime_ticks
```

로 수정된 버전입니다.

### activity_prefilter

`screening_score=max(cpu_score,fault_score,rss_score)` 동률에서 `legacy_total_score`로 다시 순서를 바꾸던 과거 tie-break를 제거했습니다. 동률이면 PID로만 deterministic ordering합니다.

### DAMON observer

과거 `1 kdamond + N targets` observer는 사용하지 않습니다. Step B/C에서 확인한 context-level region coupling을 피하기 위해 `N kdamonds + 1 target each`로 다시 작성했습니다.

## 5. 라이브러리 구조

새 핵심 파일:

- `damon_multi_observer.[ch]`: DAMON sysfs 구성 + trace parser + kdamond PID/target mapping
- `damon_profile_window.[ch]`: complete burst 조립 + 1초 WEIGHTED_NORM 32-group profile
- `stepc_damon_detector.[ch]`: protected aggregate vs candidate weighted Jaccard
- `stepc_runtime_adapter.[ch]`: 기존 `throttle_controller`로 전달
- `interference_candidate.h`: detector-controller handoff ABI

32-group 기반 파일:

- `dram_mapping.[ch]`
- `class_projector.[ch]`
- `bank_profile.[ch]`
- `addr_translate.[ch]`
- `calibration.[ch]`

## 6. 빌드/self-test

```bash
make clean
make -j
make test
```

현재 생성 시점 검증 결과:

```text
PASS: 128 raw -> 32 groups, frozen 20/400 config, independent-kdamond trace mapping
```

빌드는 `-Wall -Wextra -Wpedantic -Werror`로 수행했습니다.

## 7. 실제 protected-daemon/src에 안전하게 복사

**먼저 git status를 확인하세요.**

```bash
cd ~/cgroup_research/protected-daemon
git status --short
```

그 다음 이 묶음에서:

```bash
./apply_safe.sh ~/cgroup_research/protected-daemon/src
```

스크립트는 대상 파일을 `backup_stepc_integration_<timestamp>/`에 백업한 후 복사합니다.

자동으로 건드리지 않는 파일:

- `protected_daemon.c`
- `protected_daemon.h`
- `protected_daemon.bpf.c`
- `policy.c/h`
- `process.c/h`
- `candidate_filter.c/h`
- `cgroup.c/h`

이유: 현재 사용 중인 최신 main/policy revision을 라이브러리에서 정확히 복원할 수 없으므로 추측해서 덮어쓰지 않습니다.

## 8. main에서의 연결 순서

기존 activity prefilter가 뽑은 candidate PID 배열을 사용합니다.

개념적으로:

```c
/* protected_pids[]: candidate_filter에서 PROTECTED로 분류된 PID */
/* topk[]: activity_prefilter_select_topk()의 결과 */

for (i = 0; i < topk_count; i++)
    candidate_pids[i] = topk[i].pid;

stepc_damon_detector_config_default(&dcfg);

stepc_damon_detector_start(&det,
                           &mapping,
                           protected_pids, protected_count,
                           candidate_pids, topk_count,
                           &dcfg);

/* 실험과 같은 초기 안정화가 필요하면 최초 start/reconfigure 뒤 한 번 */
stepc_damon_detector_warmup(&det, dcfg.startup_warmup_ms);

/* 이후 steady-state decision window */
stepc_runtime_refine_and_apply(&det,
                               topk, topk_count,
                               &throttle_controller,
                               refined, refined_cap,
                               &refined_count,
                               &controller_stats);
```

### candidate set이 바뀌면

현재 구현은 target set 변경 시 detector를 stop/start하여 DAMON sysfs target set을 재구성합니다.

```text
stop -> start(new PID set) -> optional warmup -> steady windows
```

**candidate churn이 큰 실제 daemon에서 재구성/warmup을 어떻게 운영할지는 아직 별도 end-to-end 검증 항목입니다.** Step C의 6초 warmup은 실험 quality 측정을 위한 조건이었으므로, 이를 매 candidate 변화마다 production requirement로 일반화하면 안 됩니다.

## 9. 권한/소유권 전제

- `/proc/<pid>/pagemap` PFN을 읽을 권한 필요
- DAMON admin sysfs와 tracefs 쓰기 권한 필요
- prototype은 실행 중 `/sys/kernel/mm/damon/admin/kdamonds`를 독점한다고 가정함
- 다른 DAMON user와 동시 사용하지 말 것

## 10. 첫 통합 실행 권장 순서

1. 기존 daemon은 그대로 두고 새 source만 복사/빌드
2. `throttle_controller.cfg.dry_run = true`
3. protected 1 + candidate 1~몇 개의 controlled workload에서 1초 profile/J 로그 확인
4. `20/400`, 32 groups, complete burst count가 기대와 맞는지 확인
5. 그 뒤 실제 cgroup throttle 활성화
6. multi-interferer end-to-end로 넘어가기



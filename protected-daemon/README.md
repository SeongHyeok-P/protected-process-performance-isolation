# protected-daemon v2

현재 연구의 통합 런타임 소스입니다. 프로세스 lifecycle 관측, cheap activity prefilter, DAMON 기반 32-group `WEIGHTED_NORM` profile, weighted Jaccard, `A × J` 후보 점수, cgroup v2 제한 경로를 포함합니다.

## 핵심 경로

```text
candidate_filter
  -> proc_activity / activity_prefilter
  -> DAMON observer
  -> 32 page-observable recovered groups
  -> WEIGHTED_NORM profile
  -> weighted Jaccard J
  -> activity A × J
  -> throttle_controller
```

`A`는 현재 `proc_activity`의 bounded screening score이고, DAMON `nr_accesses`는 group별 활동 분포 `H`를 구성해 정규화된 spatial profile과 `J`를 만드는 데 사용합니다. 둘은 서로 다른 신호입니다.

## 빌드

`v2/Makefile`은 repository root의 `libbpf-bootstrap/`을 기본 의존 위치로 사용합니다.

```bash
cd protected-daemon/v2
make clean
make -j
make test
```

실제 daemon 빌드는 clang BPF backend, bpftool/libbpf, libelf, zlib 및 `/sys/kernel/btf/vmlinux`가 필요합니다.

## 실행

실행 시 config 파일은 보호할 Linux task `comm`을 한 줄에 하나씩 둡니다. 예시는 `../config/protected.conf.example`을 참고하십시오.

```bash
cd protected-daemon
sudo ./v2/protected_daemon ../config/protected.conf.example --dry-run
```

실제 cgroup 이동/제한은 충분한 dry-run 검증 후 `--apply`를 사용합니다. 기본 cgroup namespace는 `/sys/fs/cgroup/protected_process_isolation`로 정리했습니다.

## 외부 의존성

- Knock-Knock executable: repository root의 `Knock-Knock/main`을 기본 경로로 사용합니다.
- libbpf-bootstrap: repository root의 `libbpf-bootstrap/` 아래에 준비합니다.
- calibration output은 `/run/protected-daemon/dram-map.json`에 생성되며 Git에 포함하지 않습니다.

`protected-daemon/scripts/kk_calibrate.py`는 repository에 포함되어 있습니다. worker는 자신의 위치를 기준으로 `../../Knock-Knock/main`을 자동으로 찾을 수 있습니다.

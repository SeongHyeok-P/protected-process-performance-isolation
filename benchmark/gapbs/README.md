# GAP Benchmark Suite

GAPBS source/binary는 이 repository에 vendoring하지 않습니다. Natural-workload runner의 기본 기대 경로는:

```text
benchmark/gapbs/bfs
benchmark/gapbs/cc
benchmark/gapbs/pr
```

다른 위치에 설치했다면 `BFS_BIN`, `CC_BIN`, `PR_BIN` 환경변수를 지정할 수 있습니다.

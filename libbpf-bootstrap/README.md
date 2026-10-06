# libbpf-bootstrap dependency

`protected-daemon/v2/Makefile`은 repository root의 `libbpf-bootstrap/libbpf/src`와 `libbpf-bootstrap/bpftool/src`를 기본 경로로 사용합니다.

실제 daemon/BPF build 전에 libbpf-bootstrap checkout을 이 디렉터리에 준비하거나 Makefile 변수 `LIBBPF_SRC`, `BPFTOOL_SRC`, `BPFTOOL`을 명시하십시오.

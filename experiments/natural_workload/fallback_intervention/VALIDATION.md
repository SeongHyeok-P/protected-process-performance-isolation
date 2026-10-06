# Local validation

- `natural_victim_probe.c`: PASS (`gcc -O2 -std=gnu11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only`)
- `natural_victim_probe.c`: PASS smoke run (`1 MiB`, `50 ms` epoch; READY and VICTIM_SAMPLE output verified)
- `run_natural_fallback_intervention.sh`: PASS (`bash -n`)
- `analyze_natural_fallback.py`: PASS (`python3 -m py_compile`)
- `natural_score_probe.c`: not compiled in this sandbox because it intentionally depends on the repository-local `protected-daemon/v2/src` headers and `libstepc_integration.a`. The provided Makefile compiles and links it on the experiment machine.

The score helper uses the documented Step-C API and current activity definition `max(cpu_score, fault_score, rss_score)`.

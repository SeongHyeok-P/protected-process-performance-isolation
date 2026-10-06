#!/usr/bin/env bash
set -euo pipefail
CFLAGS='-O2 -std=gnu11 -Wall -Wextra -Wpedantic -Werror'

./verify_bundle.sh
rm -f -- *.o damon_class_observer_cli_time group_targeted_workload_v2 perf_mem_probe_diag trace_parser_selftest

gcc $CFLAGS -c damon_observer.c -o damon_observer.o
gcc $CFLAGS -c class_projector.c -o class_projector.o
gcc $CFLAGS -c calibration.c -o calibration.o
gcc $CFLAGS -c dram_mapping.c -o dram_mapping.o
gcc $CFLAGS -c damon_class_observer_cli_time.c -o damon_class_observer_cli_time.o
gcc damon_class_observer_cli_time.o damon_observer.o class_projector.o calibration.o dram_mapping.o \
    -o damon_class_observer_cli_time -lm

gcc $CFLAGS group_targeted_workload_v2.c calibration.c dram_mapping.c \
    -o group_targeted_workload_v2 -lm

gcc $CFLAGS perf_mem_probe_diag.c -o perf_mem_probe_diag
gcc $CFLAGS trace_parser_selftest.c damon_observer.c -o trace_parser_selftest

python3 -m py_compile analyze_time_windows.py analyze_positive.py window_logic_selftest.py
bash -n run_time_negative.sh run_positive_sweep.sh run_time_then_positive.sh

echo 'BUILD_OK'
./trace_parser_selftest
./window_logic_selftest.py

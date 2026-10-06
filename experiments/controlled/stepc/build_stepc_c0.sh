#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror}"
"$CC" $CFLAGS -I. \
  stepc_activity_overlap_workload.c \
  calibration.c dram_mapping.c \
  -lm -o stepc_activity_overlap_workload
echo "BUILD_STEPC_WORKLOAD_OK"

#!/usr/bin/env bash
set -euo pipefail
if [[ -n "${SHARED_DIR:-}" ]]; then :
elif [[ -d "../../stepA/stepA_time_positive" ]]; then SHARED_DIR="../../stepA/stepA_time_positive"
elif [[ -d "../stepA/stepA_time_positive" ]]; then SHARED_DIR="../stepA/stepA_time_positive"
elif [[ -d "../stepA_time_positive" ]]; then SHARED_DIR="../stepA_time_positive"
else SHARED_DIR="../../stepA/stepA_time_positive"; fi
CC="${CC:-gcc}"; CFLAGS="${CFLAGS:--std=c11 -O2 -Wall -Wextra -Wpedantic -Werror}"
for f in calibration.c calibration.h dram_mapping.c dram_mapping.h; do
  [[ -f "$SHARED_DIR/$f" ]] || { echo "[ERROR] missing $SHARED_DIR/$f" >&2; exit 1; }
done
$CC $CFLAGS -I"$SHARED_DIR" step2b_group_oracle_workload_profile.c \
  "$SHARED_DIR/calibration.c" "$SHARED_DIR/dram_mapping.c" \
  -o step2b_group_oracle_workload_profile
python3 -m py_compile analyze_profile_repeatability.py analyze_final5_j_obs.py
python3 ./selftest_profile_analyzer.py
python3 ./selftest_final5_j_obs.py
printf 'BUILD_FINAL5_ORACLE_OK\n'

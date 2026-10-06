#!/usr/bin/env bash
set -euo pipefail

# Stage 2A.1 gate: do not start semantic positive controls until the
# single-PID kernel-trace time axis is structurally sound.
./run_time_negative.sh

echo "===== TIME AXIS GATE PASSED; STARTING 4/8/16 POSITIVE CONTROL ====="
./run_positive_sweep.sh

#!/usr/bin/env bash
set -euo pipefail

SRC_DIR="$(cd "$(dirname "$0")/src" && pwd)"
DST="${1:-}"
if [[ -z "$DST" || ! -d "$DST" ]]; then
  echo "usage: $0 /path/to/protected-daemon/src" >&2
  exit 2
fi

STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$DST/backup_stepc_integration_$STAMP"
mkdir -p "$BACKUP"

# Deliberately excludes main/policy/process/candidate_filter/cgroup/protected_daemon.h.
FILES=(
  calibration.c calibration.h
  addr_translate.c addr_translate.h
  dram_mapping.c dram_mapping.h
  class_projector.c class_projector.h
  bank_profile.c bank_profile.h
  proc_activity.c proc_activity.h
  activity_prefilter.c activity_prefilter.h
  interference_candidate.h
  interference_detector.h
  throttle_controller.c throttle_controller.h
  damon_multi_observer.c damon_multi_observer.h
  damon_profile_window.c damon_profile_window.h
  stepc_damon_detector.c stepc_damon_detector.h
  stepc_runtime_adapter.c stepc_runtime_adapter.h
)

for f in "${FILES[@]}"; do
  if [[ -e "$DST/$f" ]]; then
    cp -a "$DST/$f" "$BACKUP/$f"
  fi
  cp -a "$SRC_DIR/$f" "$DST/$f"
done

echo "STEP_C_INTEGRATION_COPY=PASS"
echo "BACKUP=$BACKUP"
echo "NOTE=protected_daemon.c/policy/process/candidate_filter/cgroup were intentionally not overwritten"


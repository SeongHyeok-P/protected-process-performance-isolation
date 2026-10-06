#!/usr/bin/env bash
set -euo pipefail

fail() { echo "BUNDLE_VERIFY_FAIL: $*" >&2; exit 1; }

[[ -f PACKAGE_VERSION ]] || fail "PACKAGE_VERSION missing"
grep -qx 'stepA_time_positive_v4_verified' PACKAGE_VERSION || fail "wrong package version"

for f in damon_class_observer_cli_time.c class_projector.h class_projector.c damon_observer.h damon_observer.c; do
  [[ -f "$f" ]] || fail "$f missing"
done

grep -q 'typedef struct.*' class_projector.h || fail "class_projector.h malformed"
grep -q 'g2_region_projection_stats_t' class_projector.h || fail "old class_projector.h: projection stats API missing"
grep -q 'g2_class_projector_project_region_stats' class_projector.h || fail "old class_projector.h: project_region_stats API missing"
grep -q 'trace_ts_ns' damon_observer.h || fail "old damon_observer.h: trace_ts_ns missing"
grep -q 'trace_clock_name' damon_observer.h || fail "old damon_observer.h: trace clock fields missing"
grep -q 'damon_observer_get_parse_stats' damon_observer.h || fail "old damon_observer.h: parse stats API missing"

sha256sum -c SOURCE_MANIFEST.sha256

echo 'BUNDLE_VERIFY_OK'

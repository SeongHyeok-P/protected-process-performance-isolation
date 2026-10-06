#ifndef PROTECTED_DAEMON_STEPC_RUNTIME_ADAPTER_H
#define PROTECTED_DAEMON_STEPC_RUNTIME_ADAPTER_H

#include <stddef.h>

#include "stepc_damon_detector.h"
#include "throttle_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cooperative tick: -1 error, 0 pending, 1 decision applied.
 * Call on every main-loop iteration, not only once per second.
 * Stop on error: an apply failure may have partially modified cgroups.
 * One steady-state runtime decision window:
 *   collect frozen 1 s DAMON profile -> build interference_candidate[]
 *   -> feed the existing multi-candidate throttle controller.
 *
 * This intentionally does NOT discover candidates.  The existing
 * candidate_filter + proc_activity + activity_prefilter pipeline owns that.
 */
int stepc_runtime_refine_and_apply(
    struct stepc_damon_detector *detector,
    const struct activity_prefilter_candidate *activity_candidates,
    size_t activity_candidate_count,
    struct throttle_controller *controller,
    struct interference_candidate *scratch,
    size_t scratch_cap,
    size_t *refined_count_out,
    struct throttle_controller_stats *controller_stats);

#ifdef __cplusplus
}
#endif

#endif

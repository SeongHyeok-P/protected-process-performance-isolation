#include "stepc_runtime_adapter.h"

int stepc_runtime_refine_and_apply(
    struct stepc_damon_detector *detector,
    const struct activity_prefilter_candidate *activity_candidates,
    size_t activity_candidate_count,
    struct throttle_controller *controller,
    struct interference_candidate *scratch,
    size_t scratch_cap,
    size_t *refined_count_out,
    struct throttle_controller_stats *controller_stats)
{
    size_t refined = 0U;
    int rc;

    if (refined_count_out != NULL)
        *refined_count_out = 0U;

    if (detector == NULL || controller == NULL || scratch == NULL ||
        refined_count_out == NULL)
        return -1;

    rc = stepc_damon_detector_collect(detector);
    if (rc <= 0)
        return rc;

    if (stepc_damon_detector_build_candidates(detector,
                                               activity_candidates,
                                               activity_candidate_count,
                                               scratch,
                                               scratch_cap,
                                               &refined) != 0)
        return -1;

    if (throttle_controller_apply_candidates(controller,
                                             scratch,
                                             refined,
                                             controller_stats) !=
        THROTTLE_CONTROLLER_OK)
        return -1;

    *refined_count_out = refined;
    if (stepc_damon_detector_ack(detector) != 0)
        return -1;
    return 1;
}

#ifndef PROTECTED_DAEMON_STEPC_DAMON_DETECTOR_H
#define PROTECTED_DAEMON_STEPC_DAMON_DETECTOR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "activity_prefilter.h"
#include "damon_multi_observer.h"
#include "damon_profile_window.h"
#include "interference_candidate.h"

#ifdef __cplusplus
extern "C" {
#endif

enum stepc_score_mode {
    STEPC_SCORE_ACTIVITY_X_OVERLAP = 0,
    STEPC_SCORE_OVERLAP_ONLY = 1
};

struct stepc_damon_detector_config {
    struct damon_runtime_config damon;
    unsigned int window_ms;
    unsigned int startup_warmup_ms;
    double overlap_score_threshold;
    double throttle_score_threshold;
    enum stepc_score_mode score_mode;
};

struct stepc_damon_detector {
    struct stepc_damon_detector_config cfg;
    const struct dram_mapping *mapping;
    struct damon_multi_observer observer;
    struct damon_profile_window profiles;

    pid_t target_pids[DAMON_MULTI_MAX_TARGETS];
    size_t protected_count;
    size_t candidate_count;
    size_t target_count;
    int started;
};

void stepc_damon_detector_config_default(struct stepc_damon_detector_config *cfg);
int stepc_damon_detector_start(struct stepc_damon_detector *det,
                               const struct dram_mapping *mapping,
                               const pid_t *protected_pids,
                               size_t protected_count,
                               const pid_t *candidate_pids,
                               size_t candidate_count,
                               const struct stepc_damon_detector_config *cfg);
int stepc_damon_detector_warmup(struct stepc_damon_detector *det,
                                unsigned int warmup_ms);
int stepc_damon_detector_collect(struct stepc_damon_detector *det);
int stepc_damon_detector_build_candidates(
    const struct stepc_damon_detector *det,
    const struct activity_prefilter_candidate *activity_candidates,
    size_t activity_candidate_count,
    struct interference_candidate *out,
    size_t out_cap,
    size_t *out_count);
int stepc_damon_detector_stop(struct stepc_damon_detector *det);

#ifdef __cplusplus
}
#endif

#endif


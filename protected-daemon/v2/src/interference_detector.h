#ifndef INTERFERENCE_DETECTOR_H
#define INTERFERENCE_DETECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "bank_profile.h"
#include "dram_mapping.h"
#include "proc_activity.h"
#include "interference_candidate.h"

#define INTERFERENCE_DETECTOR_REASON_MAX 256U

#ifdef __cplusplus
extern "C" {
#endif

enum interference_detector_rc {
    INTERFERENCE_DETECTOR_OK = 0,
    INTERFERENCE_DETECTOR_ERR_INVALID_ARG = -1,
    INTERFERENCE_DETECTOR_ERR_NO_MAPPING = -2,
    INTERFERENCE_DETECTOR_ERR_NO_MEMORY = -3,
    INTERFERENCE_DETECTOR_ERR_NO_PROTECTED_PROFILE = -4,
    INTERFERENCE_DETECTOR_ERR_PROFILE = -5,
    INTERFERENCE_DETECTOR_ERR_ACTIVITY = -6
};

enum interference_process_role {
    INTERFERENCE_PROCESS_NORMAL = 0,
    INTERFERENCE_PROCESS_PROTECTED = 1
};

struct interference_process_state {
    pid_t pid;
    enum interference_process_role role;
    bool alive;

    bool has_activity_sample;
    struct proc_activity_sample activity_sample;
};

struct interference_detector_config {
    size_t max_normal_candidates;

    double activity_score_threshold;
    double overlap_score_threshold;
    double throttle_score_threshold;

    struct proc_activity_config activity_config;
    struct bank_profile_config profile_config;
};

/* struct interference_candidate is defined in interference_candidate.h. */

struct interference_detector_stats {
    size_t process_count;
    size_t protected_processes;
    size_t normal_processes;

    size_t protected_profiles_built;
    size_t activity_samples;
    size_t active_normal_candidates;
    size_t normal_profiles_built;
    size_t output_candidates;

    size_t activity_errors;
    size_t profile_errors;

    int last_error;
    char last_error_reason[INTERFERENCE_DETECTOR_REASON_MAX];
};

void interference_detector_config_default(
    struct interference_detector_config *cfg);

void interference_process_state_reset(
    struct interference_process_state *state);

void interference_candidate_reset(
    struct interference_candidate *candidate);

void interference_detector_stats_reset(
    struct interference_detector_stats *stats);

int interference_detector_run_once(
    struct interference_process_state *processes,
    size_t process_count,
    const struct dram_mapping *mapping,
    const struct interference_detector_config *cfg,
    struct interference_candidate *out,
    size_t out_cap,
    size_t *out_count,
    struct interference_detector_stats *stats);

const char *interference_detector_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* INTERFERENCE_DETECTOR_H */


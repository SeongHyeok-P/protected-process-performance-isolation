#ifndef THROTTLE_CONTROLLER_H
#define THROTTLE_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "cgroup.h"
#include "interference_candidate.h"

#define THROTTLE_CONTROLLER_VALUE_MAX 64U
#define THROTTLE_CONTROLLER_REASON_MAX 256U
#define THROTTLE_CONTROLLER_API_VERSION 0x00010001U

enum throttle_controller_rc {
    THROTTLE_CONTROLLER_OK = 0,
    THROTTLE_CONTROLLER_ERR_INVALID_ARG = -1,
    THROTTLE_CONTROLLER_ERR_NO_MEMORY = -2,
    THROTTLE_CONTROLLER_ERR_NO_SPACE = -3,
    THROTTLE_CONTROLLER_ERR_TIME = -4,
    THROTTLE_CONTROLLER_ERR_CGROUP = -5,
    THROTTLE_CONTROLLER_ERR_NOT_INITIALIZED = -6
};

enum throttle_state {
    THROTTLE_STATE_NONE = 0,
    THROTTLE_STATE_THROTTLED = 1,
    THROTTLE_STATE_COOLDOWN = 2
};

/*
 * Optional clock injection for deterministic unit tests.
 * Return THROTTLE_CONTROLLER_OK and store a CLOCK_MONOTONIC-like timestamp
 * in *now_ns.  Production code normally leaves clock_fn == NULL.
 */
typedef int (*throttle_controller_clock_fn)(void *clock_ctx,
                                            uint64_t *now_ns);

struct throttle_controller_config {
    enum proc_group normal_group;
    enum proc_group throttled_group;
    enum proc_group protected_group;

    char throttled_cpu_max[THROTTLE_CONTROLLER_VALUE_MAX];
    char protected_memory_low[THROTTLE_CONTROLLER_VALUE_MAX];

    double high_score_threshold;
    double low_score_threshold;

    uint64_t min_throttle_duration_ms;
    uint64_t cooldown_duration_ms;
    uint64_t stale_after_ms;

    size_t max_records;

    bool dry_run;
    bool configure_throttled_cpu_max;
    bool configure_protected_memory_low;

    throttle_controller_clock_fn clock_fn;
    void *clock_ctx;
};

struct throttle_record {
    pid_t pid;
    enum throttle_state state;

    uint64_t first_seen_ns;
    uint64_t last_seen_ns;
    uint64_t state_entered_ns;
    uint64_t last_action_ns;

    double last_activity_score;
    double last_overlap_score;
    double last_final_score;
    uint64_t last_common_pages;

    bool seen_in_last_apply;
    bool last_should_throttle;

    int last_error;
    char reason[THROTTLE_CONTROLLER_REASON_MAX];
};

struct throttle_controller {
    bool initialized;
    struct throttle_controller_config cfg;

    struct throttle_record *records;
    size_t record_count;
    size_t record_cap;
};

struct throttle_controller_stats {
    size_t candidate_count;
    size_t records_seen;
    size_t throttle_actions;
    size_t release_actions;
    size_t kept_throttled;
    size_t skipped_low_score;
    size_t skipped_cooldown;
    size_t stale_releases;
    size_t errors;

    int last_error;
    char last_error_reason[THROTTLE_CONTROLLER_REASON_MAX];
};

uint32_t throttle_controller_api_version(void);

void throttle_controller_config_default(
    struct throttle_controller_config *cfg);

void throttle_controller_stats_reset(
    struct throttle_controller_stats *stats);

void throttle_controller_record_reset(
    struct throttle_record *record);

int throttle_controller_init(
    struct throttle_controller *controller,
    const struct throttle_controller_config *cfg);

void throttle_controller_destroy(
    struct throttle_controller *controller);

int throttle_controller_apply_candidates(
    struct throttle_controller *controller,
    const struct interference_candidate *candidates,
    size_t candidate_count,
    struct throttle_controller_stats *stats);

int throttle_controller_release_all(
    struct throttle_controller *controller,
    struct throttle_controller_stats *stats);

bool throttle_controller_get_record(
    const struct throttle_controller *controller,
    pid_t pid,
    struct throttle_record *out);

const char *throttle_state_str(enum throttle_state state);
const char *throttle_controller_strerror(int rc);

#endif /* THROTTLE_CONTROLLER_H */

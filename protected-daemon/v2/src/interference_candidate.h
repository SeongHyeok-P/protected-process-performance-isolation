#ifndef PROTECTED_DAEMON_INTERFERENCE_CANDIDATE_H
#define PROTECTED_DAEMON_INTERFERENCE_CANDIDATE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define INTERFERENCE_DETECTOR_REASON_MAX 256U

/*
 * Stable hand-off object between a detector/refinement stage and the existing
 * throttle_controller.  common_pages is retained for ABI compatibility; the
 * DAMON WEIGHTED_NORM path leaves it at 0 because overlap is a distributional
 * weighted-Jaccard score rather than a static common-page count.
 */
struct interference_candidate {
    pid_t pid;

    double activity_score;
    double cpu_score;
    double fault_score;
    double rss_score;

    double overlap_score;
    double final_score;
    uint64_t common_pages;

    bool active;
    bool should_throttle;

    char reason[INTERFERENCE_DETECTOR_REASON_MAX];
};

#endif

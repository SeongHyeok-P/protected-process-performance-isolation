#ifndef PROTECTED_DAEMON_DAMON_PROFILE_WINDOW_H
#define PROTECTED_DAEMON_DAMON_PROFILE_WINDOW_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "class_projector.h"
#include "damon_multi_observer.h"
#include "dram_mapping.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DAMON_PROFILE_BURST_GAP_NS 30000000ULL /* 30 ms: frozen analysis rule */

struct damon_profile_target {
    pid_t pid;
    int pagemap_fd;
    g2_class_projector_t projector;

    uint64_t complete_bursts;
    uint64_t discarded_bursts;
    uint64_t projected_regions;
};

struct damon_burst_state {
    int active;
    uint64_t first_ts_ns;
    uint64_t last_ts_ns;
    unsigned int expected_regions;
    unsigned int seen_regions;
    int consistent;

    struct damon_multi_sample *regions;
    size_t region_count;
    size_t region_cap;
};

struct damon_profile_window {
    const struct dram_mapping *mapping;
    struct damon_profile_target *targets;
    struct damon_burst_state *bursts;
    size_t target_count;
    size_t group_count;

    uint64_t window_start_ns;
    uint64_t window_end_ns;
    int window_active;
};

int damon_profile_window_init(struct damon_profile_window *pw,
                              const struct dram_mapping *mapping,
                              const pid_t *pids,
                              size_t target_count);
void damon_profile_window_destroy(struct damon_profile_window *pw);
void damon_profile_window_begin(struct damon_profile_window *pw,
                                uint64_t start_ns,
                                uint64_t end_ns);
void damon_profile_window_on_sample(const struct damon_multi_sample *sample,
                                    void *user_data);
/* Flushes any burst that is already structurally complete. */
void damon_profile_window_flush(struct damon_profile_window *pw);

int damon_profile_weighted_jaccard(const struct damon_profile_window *pw,
                                   const size_t *protected_indices,
                                   size_t protected_count,
                                   size_t candidate_index,
                                   double *jaccard_out);

uint64_t damon_profile_activity_units(const struct damon_profile_window *pw,
                                      size_t target_index);

#ifdef __cplusplus
}
#endif

#endif


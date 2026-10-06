#ifndef DAEMON_RUNTIME_H
#define DAEMON_RUNTIME_H
#include "stepc_runtime_adapter.h"
struct daemon_runtime {
    struct stepc_damon_detector detector;
    struct throttle_controller controller;
    const struct dram_mapping *mapping;
    struct proc_activity_sample *previous;
    size_t previous_count;
    pid_t protected_pids[DAMON_MULTI_MAX_TARGETS];
    size_t protected_count;
    struct activity_prefilter_candidate top[DAMON_MULTI_MAX_TARGETS];
    size_t top_count;
    uint64_t target_birth[DAMON_MULTI_MAX_TARGETS];
    uint64_t last_refresh_ns;
    pid_t daemon_pid;
    int dry_run, invalidated, have_snapshot;
};
int daemon_runtime_init(struct daemon_runtime *,const struct dram_mapping *,pid_t,int);
void daemon_runtime_invalidate_pid(struct daemon_runtime *,pid_t);
int daemon_runtime_tick(struct daemon_runtime *);
int daemon_runtime_destroy(struct daemon_runtime *);
#endif

#ifndef PROTECTED_DAEMON_DAMON_MULTI_OBSERVER_H
#define PROTECTED_DAEMON_DAMON_MULTI_OBSERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAMON_MULTI_MAX_TARGETS 64U
#define DAMON_MULTI_TRACE_CLOCK_MAX 32U
#define DAMON_MULTI_READBUF 65536U

struct damon_runtime_config {
    unsigned long sample_us;
    unsigned long aggr_us;
    unsigned long update_us;
    unsigned int min_regions;
    unsigned int max_regions;
};

struct damon_multi_sample {
    size_t target_index;
    pid_t target_pid;
    pid_t kdamond_pid;
    unsigned int nr_regions;
    uint64_t start;
    uint64_t end;
    unsigned int nr_accesses;
    unsigned int age;
    uint64_t trace_ts_ns;
};

typedef void (*damon_multi_callback)(const struct damon_multi_sample *sample,
                                     void *user_data);

struct damon_multi_observer {
    struct damon_runtime_config cfg;
    pid_t target_pids[DAMON_MULTI_MAX_TARGETS];
    pid_t kdamond_pids[DAMON_MULTI_MAX_TARGETS];
    size_t target_count;

    int trace_fd;
    int started;
    int trace_event_previous;
    int trace_event_previous_valid;
    int trace_clock_changed;
    char trace_clock_previous[DAMON_MULTI_TRACE_CLOCK_MAX];

    char readbuf[DAMON_MULTI_READBUF];
    size_t readbuf_used;
    uint64_t trace_lines;
    uint64_t matched_lines;
    uint64_t parse_fail_lines;
};

void damon_runtime_config_frozen_stepc(struct damon_runtime_config *cfg);
void damon_multi_observer_reset(struct damon_multi_observer *obs);
int damon_multi_observer_start(struct damon_multi_observer *obs,
                               const pid_t *target_pids,
                               size_t target_count,
                               const struct damon_runtime_config *cfg);
int damon_multi_observer_poll(struct damon_multi_observer *obs,
                              int timeout_ms,
                              damon_multi_callback cb,
                              void *user_data);
int damon_multi_observer_stop(struct damon_multi_observer *obs);
void damon_multi_observer_destroy(struct damon_multi_observer *obs);

/* Pure parser helper for self-tests. */
int damon_multi_parse_trace_line_for_test(
    const char *line,
    const pid_t *kdamond_pids,
    const pid_t *target_pids,
    size_t target_count,
    struct damon_multi_sample *out);

#ifdef __cplusplus
}
#endif

#endif


#ifndef DAMON_OBSERVER_H
#define DAMON_OBSERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DAMON_OBSERVER_MAX_TARGETS 64U
#define DAMON_OBSERVER_READBUF 65536U

typedef struct {
    unsigned long sample_us;
    unsigned long aggr_us;
    unsigned long update_us;
    unsigned int min_regions;
    unsigned int max_regions;
} damon_observer_config_t;

typedef struct {
    unsigned int target_id;
    pid_t pid;
    unsigned int nr_regions;
    uint64_t start;
    uint64_t end;
    unsigned int nr_accesses;
    unsigned int age;
    uint64_t trace_ts_ns; /* kernel trace timestamp from trace_pipe */
} damon_region_sample_t;

typedef void (*damon_observer_callback_t)(const damon_region_sample_t *, void *);

typedef struct {
    damon_observer_config_t cfg;
    pid_t pids[DAMON_OBSERVER_MAX_TARGETS];
    size_t nr_targets;
    int trace_fd;
    int started;
    char trace_clock_name[32];
    char trace_clock_previous[32];
    int trace_clock_changed;
    char readbuf[DAMON_OBSERVER_READBUF];
    size_t readbuf_used;
    uint64_t trace_lines;
    uint64_t matched_lines;
    uint64_t parse_fail_lines;
} damon_observer_t;

void damon_observer_config_default(damon_observer_config_t *cfg);
int damon_observer_init(damon_observer_t *obs,
                        const damon_observer_config_t *cfg,
                        const pid_t *pids,
                        size_t nr_targets);
int damon_observer_start(damon_observer_t *obs);
int damon_observer_poll(damon_observer_t *obs,
                        int timeout_ms,
                        damon_observer_callback_t cb,
                        void *user_data);
int damon_observer_stop(damon_observer_t *obs);
void damon_observer_destroy(damon_observer_t *obs);
int damon_observer_parse_trace_line_for_test(const char *line,
                                              const pid_t *pids,
                                              size_t nr_targets,
                                              damon_region_sample_t *sample);

void damon_observer_get_parse_stats(const damon_observer_t *obs,
                                    uint64_t *trace_lines,
                                    uint64_t *matched_lines,
                                    uint64_t *parse_fail_lines);

#endif

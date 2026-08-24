#ifndef DAMON_OBSERVER_H
#define DAMON_OBSERVER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DAMON_OBSERVER_MAX_TARGETS 64

typedef struct {
    unsigned long sample_us;
    unsigned long aggr_us;
    unsigned long update_us;
    unsigned int min_regions;
    unsigned int max_regions;
    const char *admin_root;
    const char *trace_root;
} damon_observer_config_t;

typedef struct {
    unsigned int target_id;
    pid_t pid;
    unsigned int nr_regions;
    uint64_t start;
    uint64_t end;
    unsigned int nr_accesses;
    unsigned int age;
} damon_region_sample_t;

typedef void (*damon_region_callback_t)(const damon_region_sample_t *sample,
                                        void *user_data);

typedef struct {
    damon_observer_config_t cfg;
    pid_t pids[DAMON_OBSERVER_MAX_TARGETS];
    size_t nr_pids;
    int trace_fd;
    int running;
    char read_buf[65536];
    size_t read_len;
} damon_observer_t;

void damon_observer_config_default(damon_observer_config_t *cfg);
int damon_observer_init(damon_observer_t *obs,
                        const damon_observer_config_t *cfg,
                        const pid_t *pids,
                        size_t nr_pids);
int damon_observer_start(damon_observer_t *obs);
int damon_observer_poll(damon_observer_t *obs,
                        int timeout_ms,
                        damon_region_callback_t cb,
                        void *user_data);
int damon_observer_stop(damon_observer_t *obs);
void damon_observer_destroy(damon_observer_t *obs);

/* Exposed for parser unit testing. Returns 1 on success, 0 if not a DAMON line. */
int damon_observer_parse_trace_line(const char *line,
                                    const pid_t *pids,
                                    size_t nr_pids,
                                    damon_region_sample_t *out);

#endif


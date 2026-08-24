#define _POSIX_C_SOURCE 200809L
#include "damon_observer.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -1;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

typedef struct {
    long long t0_ms;
} print_ctx_t;

static void print_region(const damon_region_sample_t *s, void *user_data)
{
    print_ctx_t *ctx = user_data;
    long long now = monotonic_ms();
    long long rel = (now >= 0 && ctx->t0_ms >= 0) ? now - ctx->t0_ms : -1;

    printf("REGION,%lld,%u,%d,%u,0x%llx,0x%llx,%llu,%u,%u\n",
           rel,
           s->target_id,
           (int)s->pid,
           s->nr_regions,
           (unsigned long long)s->start,
           (unsigned long long)s->end,
           (unsigned long long)(s->end - s->start),
           s->nr_accesses,
           s->age);
    fflush(stdout);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: sudo %s [--duration SEC] PID [PID ...]\n"
        "\n"
        "Stage 7-A DAMON observer only. No prefilter/ranking/throttling.\n",
        prog);
}

int main(int argc, char **argv)
{
    damon_observer_config_t cfg;
    damon_observer_t obs;
    pid_t pids[DAMON_OBSERVER_MAX_TARGETS];
    size_t nr_pids = 0;
    int duration_sec = 10;
    int i;
    long long deadline;
    print_ctx_t print_ctx;

    damon_observer_config_default(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0) {
            char *end = NULL;
            long v;
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            errno = 0;
            v = strtol(argv[i], &end, 10);
            if (errno || !end || *end != '\0' || v <= 0 || v > 86400) {
                fprintf(stderr, "invalid duration: %s\n", argv[i]);
                return 2;
            }
            duration_sec = (int)v;
        } else {
            char *end = NULL;
            long v;
            if (nr_pids >= DAMON_OBSERVER_MAX_TARGETS) {
                fprintf(stderr, "too many PIDs\n");
                return 2;
            }
            errno = 0;
            v = strtol(argv[i], &end, 10);
            if (errno || !end || *end != '\0' || v <= 0) {
                fprintf(stderr, "invalid PID: %s\n", argv[i]);
                return 2;
            }
            pids[nr_pids++] = (pid_t)v;
        }
    }

    if (nr_pids == 0) {
        usage(argv[0]);
        return 2;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "run as root (DAMON sysfs/tracefs requires privilege)\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (damon_observer_init(&obs, &cfg, pids, nr_pids) < 0) {
        perror("damon_observer_init");
        return 1;
    }
    if (damon_observer_start(&obs) < 0) {
        perror("damon_observer_start");
        damon_observer_destroy(&obs);
        return 1;
    }

    printf("# Stage7-A DAMON observer\n");
    printf("# sample_us=%lu aggr_us=%lu update_us=%lu min_regions=%u max_regions=%u targets=%zu\n",
           cfg.sample_us, cfg.aggr_us, cfg.update_us,
           cfg.min_regions, cfg.max_regions, nr_pids);
    printf("# nr_accesses theoretical max per aggregation ~= aggr_us/sample_us = %lu\n",
           cfg.sample_us ? cfg.aggr_us / cfg.sample_us : 0);
    printf("TYPE,elapsed_ms,target_id,pid,nr_regions,start,end,size_bytes,nr_accesses,age\n");
    fflush(stdout);

    print_ctx.t0_ms = monotonic_ms();
    deadline = print_ctx.t0_ms + (long long)duration_sec * 1000LL;

    while (!g_stop) {
        long long now = monotonic_ms();
        int rc;
        if (now >= 0 && now >= deadline)
            break;
        rc = damon_observer_poll(&obs, 250, print_region, &print_ctx);
        if (rc < 0) {
            perror("damon_observer_poll");
            break;
        }
    }

    if (damon_observer_stop(&obs) < 0)
        perror("damon_observer_stop");
    damon_observer_destroy(&obs);
    return 0;
}


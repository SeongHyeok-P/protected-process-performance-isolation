#define _POSIX_C_SOURCE 200809L
#include "calibration.h"
#include "class_projector.h"
#include "damon_observer.h"
#include "dram_mapping.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CALIBRATION_PATH "/run/protected-daemon/dram-map.json"

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
    g2_class_projector_t *projector;
    int pagemap_fd;
    int projection_failed;

    unsigned long aggr_us;
    unsigned int aggs_per_window;
    unsigned int aggs_in_window;
    unsigned int regions_seen_in_agg;
    unsigned int regions_expected_in_agg;
    uint64_t window_index;
} run_ctx_t;

static void flush_window(run_ctx_t *ctx, int complete)
{
    uint64_t start_ms;
    uint64_t duration_ms;
    uint64_t end_ms;

    if (ctx == NULL || ctx->projector == NULL || ctx->aggs_in_window == 0U)
        return;

    start_ms = ctx->window_index *
               ((uint64_t)ctx->aggs_per_window * (uint64_t)ctx->aggr_us / 1000ULL);
    duration_ms = (uint64_t)ctx->aggs_in_window *
                  (uint64_t)ctx->aggr_us / 1000ULL;
    end_ms = start_ms + duration_ms;

    g2_class_projector_print_window_report(ctx->projector,
                                           stdout,
                                           ctx->window_index,
                                           start_ms,
                                           end_ms,
                                           ctx->aggs_in_window,
                                           complete);
    fflush(stdout);
    g2_class_projector_reset_window(ctx->projector);
    ++ctx->window_index;
    ctx->aggs_in_window = 0U;
}

static void account_aggregation_boundary(run_ctx_t *ctx,
                                         const damon_region_sample_t *s)
{
    if (ctx->regions_seen_in_agg == 0U) {
        ctx->regions_expected_in_agg = s->nr_regions;
        if (ctx->regions_expected_in_agg == 0U)
            return;
    }

    ++ctx->regions_seen_in_agg;
    if (ctx->regions_seen_in_agg < ctx->regions_expected_in_agg)
        return;

    ctx->regions_seen_in_agg = 0U;
    ctx->regions_expected_in_agg = 0U;
    ++ctx->aggs_in_window;

    if (ctx->aggs_in_window >= ctx->aggs_per_window)
        flush_window(ctx, 1);
}

static void observe_and_project(const damon_region_sample_t *s, void *user_data)
{
    run_ctx_t *ctx = user_data;
    long long now = monotonic_ms();
    long long rel =
        (now >= 0 && ctx->t0_ms >= 0) ? now - ctx->t0_ms : -1;

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

    if (!ctx->projection_failed &&
        g2_class_projector_project_region(ctx->projector,
                                          ctx->pagemap_fd,
                                          s->start,
                                          s->end,
                                          s->nr_accesses) < 0) {
        fprintf(stderr,
                "[ERROR] project region 0x%llx-0x%llx: %s\n",
                (unsigned long long)s->start,
                (unsigned long long)s->end,
                strerror(errno));
        ctx->projection_failed = 1;
        g_stop = 1;
        return;
    }

    account_aggregation_boundary(ctx, s);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: sudo %s [--duration SEC] [--hot-threshold N] "
            "[--calibration PATH] PID\n\n"
            "G2 DAMON -> VA -> pagemap -> PA -> recovered-class projection.\n"
            "Default DAMON min_regions is fixed at 128 for this validation.\n",
            prog);
}

static int parse_positive_int(const char *s, long min_v, long max_v, long *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || out == NULL)
        return -1;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min_v || v > max_v)
        return -1;

    *out = v;
    return 0;
}

static int load_mapping(const char *path,
                        struct calibration_result *calibration,
                        struct dram_mapping *mapping)
{
    int rc;
    size_t i;

    calibration_result_reset(calibration);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(path, calibration);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr,
                "[ERROR] calibration_load_result(%s): %s\n",
                path,
                calibration_strerror(rc));
        return -1;
    }

    rc = dram_mapping_init_from_calibration(mapping, calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr,
                "[ERROR] dram_mapping_init_from_calibration: %s\n",
                dram_mapping_strerror(rc));
        return -1;
    }

    printf("# calibration=%s\n", path);
    printf("# mask_set_id=%s mask_count=%zu\n",
           mapping->mask_set_id,
           mapping->mask_count);
    for (i = 0U; i < mapping->mask_count; ++i)
        printf("# mask[%zu]=0x%016" PRIx64 "\n",
               i,
               mapping->masks[i]);

    return 0;
}

int main(int argc, char **argv)
{
    damon_observer_config_t cfg;
    damon_observer_t obs;
    struct calibration_result calibration;
    struct dram_mapping mapping;
    g2_class_projector_t projector;
    run_ctx_t ctx;
    const char *calibration_path = DEFAULT_CALIBRATION_PATH;
    int duration_sec = 30;
    unsigned int hot_threshold = 10U;
    pid_t pid = -1;
    long page_size_l;
    int pagemap_fd = -1;
    char pagemap_path[128];
    long long deadline;
    int i;
    int rc_main = 0;

    memset(&projector, 0, sizeof(projector));
    memset(&ctx, 0, sizeof(ctx));
    damon_observer_config_default(&cfg);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration") == 0) {
            long v;
            if (++i >= argc ||
                parse_positive_int(argv[i], 1, 86400, &v) != 0) {
                usage(argv[0]);
                return 2;
            }
            duration_sec = (int)v;
        } else if (strcmp(argv[i], "--hot-threshold") == 0) {
            long v;
            if (++i >= argc ||
                parse_positive_int(argv[i], 1, 1000000, &v) != 0) {
                usage(argv[0]);
                return 2;
            }
            hot_threshold = (unsigned int)v;
        } else if (strcmp(argv[i], "--calibration") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            calibration_path = argv[i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            long v;
            if (pid > 0 ||
                parse_positive_int(argv[i], 1, 2147483647L, &v) != 0) {
                usage(argv[0]);
                return 2;
            }
            pid = (pid_t)v;
        }
    }

    if (pid <= 0) {
        usage(argv[0]);
        return 2;
    }

    if (geteuid() != 0) {
        fprintf(stderr,
                "run as root: DAMON sysfs/tracefs and pagemap PFN access "
                "require privilege\n");
        return 1;
    }

    if (load_mapping(calibration_path, &calibration, &mapping) < 0)
        return 1;

    page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return 1;
    }

    if (g2_class_projector_init(&projector,
                                &mapping,
                                (size_t)page_size_l,
                                hot_threshold) < 0) {
        perror("g2_class_projector_init");
        return 1;
    }

    if (snprintf(pagemap_path,
                 sizeof(pagemap_path),
                 "/proc/%d/pagemap",
                 (int)pid) >= (int)sizeof(pagemap_path)) {
        fprintf(stderr, "pagemap path too long\n");
        g2_class_projector_destroy(&projector);
        return 1;
    }

    pagemap_fd = open(pagemap_path, O_RDONLY | O_CLOEXEC);
    if (pagemap_fd < 0) {
        perror("open(pagemap)");
        g2_class_projector_destroy(&projector);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (damon_observer_init(&obs, &cfg, &pid, 1U) < 0) {
        perror("damon_observer_init");
        close(pagemap_fd);
        g2_class_projector_destroy(&projector);
        return 1;
    }

    if (damon_observer_start(&obs) < 0) {
        perror("damon_observer_start");
        damon_observer_destroy(&obs);
        close(pagemap_fd);
        g2_class_projector_destroy(&projector);
        return 1;
    }

    printf("# Stage6-G2 DAMON recovered-class observer\n");
    printf("# pid=%d duration_sec=%d\n", (int)pid, duration_sec);
    printf("# sample_us=%lu aggr_us=%lu update_us=%lu "
           "min_regions=%u max_regions=%u\n",
           cfg.sample_us,
           cfg.aggr_us,
           cfg.update_us,
           cfg.min_regions,
           cfg.max_regions);
    printf("# nr_accesses theoretical max per aggregation ~= %lu\n",
           cfg.sample_us != 0U ? cfg.aggr_us / cfg.sample_us : 0U);
    printf("# WEIGHTED=legacy page-replicated estimate: every present page "
           "inherits region nr_accesses\n");
    printf("# WEIGHTED_NORM=region-normalized estimate: each region gets total "
           "budget nr_accesses, then splits it by recovered-class "
           "composition\n");
    printf("# WEIGHTED_NORM fixed-point scale=%llu units per nr_accesses=1\n",
           (unsigned long long)G2_NORM_SCALE);
    printf("# IMPORTANT: weighted units are estimates, not hardware load/store "
           "or DRAM-request counts\n");
    printf("# hot_threshold=%u\n", hot_threshold);
    printf("# window_ms=1000 aggregation_based=yes aggregations_per_window=%lu\n",
           cfg.aggr_us != 0U ? 1000000UL / cfg.aggr_us : 0UL);
    printf("# page_class_deltas=%zu lines_per_page=%zu class_count=%zu\n",
           projector.nr_line_deltas,
           projector.lines_per_page,
           projector.class_count);
    printf("TYPE,elapsed_ms,target_id,pid,nr_regions,start,end,"
           "size_bytes,nr_accesses,age\n");
    fflush(stdout);

    ctx.t0_ms = monotonic_ms();
    ctx.projector = &projector;
    ctx.pagemap_fd = pagemap_fd;
    ctx.projection_failed = 0;
    ctx.aggr_us = cfg.aggr_us;
    ctx.aggs_per_window =
        (cfg.aggr_us != 0U) ? (unsigned int)(1000000UL / cfg.aggr_us) : 0U;
    if (ctx.aggs_per_window == 0U)
        ctx.aggs_per_window = 1U;
    ctx.aggs_in_window = 0U;
    ctx.regions_seen_in_agg = 0U;
    ctx.regions_expected_in_agg = 0U;
    ctx.window_index = 0U;

    deadline = ctx.t0_ms + (long long)duration_sec * 1000LL;

    while (!g_stop) {
        long long now = monotonic_ms();
        int rc;

        if (now >= 0 && now >= deadline)
            break;

        rc = damon_observer_poll(&obs,
                                 250,
                                 observe_and_project,
                                 &ctx);
        if (rc < 0) {
            perror("damon_observer_poll");
            rc_main = 1;
            break;
        }
    }

    if (damon_observer_stop(&obs) < 0) {
        perror("damon_observer_stop");
        rc_main = 1;
    }

    if (ctx.projection_failed)
        rc_main = 1;

    if (ctx.aggs_in_window > 0U || ctx.regions_seen_in_agg > 0U) {
        if (ctx.regions_seen_in_agg > 0U && ctx.aggs_in_window == 0U)
            ctx.aggs_in_window = 1U;
        flush_window(&ctx, 0);
    }

    g2_class_projector_print_report(&projector, stdout);
    fflush(stdout);

    damon_observer_destroy(&obs);
    close(pagemap_fd);
    g2_class_projector_destroy(&projector);

    return rc_main;
}

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
#define DAMON_ADMIN_STATE_PATH "/sys/kernel/mm/damon/admin/kdamonds/0/state"

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0U;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct window_diag {
    uint64_t hot_regions;
    uint64_t low_regions;
    uint64_t cold_regions;
    uint64_t hot_present_pages;
    uint64_t low_present_pages;
    uint64_t cold_present_pages;
    uint64_t hot_workset_span_pages;
    uint64_t low_workset_span_pages;
    uint64_t cold_workset_span_pages;
};

typedef struct {
    g2_class_projector_t *projector;
    int pagemap_fd;
    size_t page_size;
    unsigned int hot_threshold;
    int projection_failed;
    int boundary_failed;

    uint64_t callback_t0_ns;
    unsigned int warmup_ms;
    unsigned int window_ms;

    int have_workset;
    int restrict_workset;
    uint64_t workset_start;
    uint64_t workset_end;

    int boundary_synced;
    int have_last_region_start;
    uint64_t last_region_start;
    uint64_t dropped_initial_regions;

    damon_region_sample_t *agg_samples;
    size_t agg_capacity;
    size_t agg_count;
    unsigned int agg_expected;
    uint64_t agg_first_callback_ns;
    uint64_t agg_last_callback_ns;

    uint64_t sync_trace_ts_ns;
    uint64_t sync_callback_ns;
    uint64_t warmup_end_ts_ns;
    uint64_t completed_aggregations;
    uint64_t warmup_dropped_aggregations;
    uint64_t previous_agg_ts_ns;

    int have_window;
    uint64_t current_window_bin;
    uint64_t window_index;
    unsigned int window_aggregations;
    struct window_diag window_diag;
} run_ctx_t;

static uint64_t page_span_overlap(uint64_t start, uint64_t end,
                                  uint64_t ws_start, uint64_t ws_end,
                                  size_t page_size)
{
    uint64_t lo = start > ws_start ? start : ws_start;
    uint64_t hi = end < ws_end ? end : ws_end;
    uint64_t first, last;
    if (hi <= lo || page_size == 0U)
        return 0U;
    first = lo / (uint64_t)page_size;
    last = (hi + (uint64_t)page_size - 1U) / (uint64_t)page_size;
    return last > first ? last - first : 0U;
}

static void reset_window_diag(struct window_diag *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void print_window_diag(const run_ctx_t *ctx,
                              uint64_t start_ms,
                              uint64_t end_ms)
{
    const struct window_diag *d = &ctx->window_diag;
    uint64_t hot_span = d->hot_workset_span_pages;
    uint64_t low_span = d->low_workset_span_pages;
    uint64_t cold_span = d->cold_workset_span_pages;

    printf("WINDOW_ACTIVITY_DIAG,%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",hot_regions=%" PRIu64 ",low_regions=%" PRIu64
           ",cold_regions=%" PRIu64 ",hot_present_pages=%" PRIu64
           ",low_present_pages=%" PRIu64 ",cold_present_pages=%" PRIu64
           ",hot_workset_span_pages=%" PRIu64
           ",low_workset_span_pages=%" PRIu64
           ",cold_workset_span_pages=%" PRIu64 "\n",
           ctx->window_index, start_ms, end_ms,
           d->hot_regions, d->low_regions, d->cold_regions,
           d->hot_present_pages, d->low_present_pages, d->cold_present_pages,
           hot_span, low_span, cold_span);
}

static void flush_time_window(run_ctx_t *ctx, int complete)
{
    uint64_t window_ns;
    uint64_t trace_start_ns;
    uint64_t trace_end_ns;
    uint64_t start_ms;
    uint64_t end_ms;

    if (ctx == NULL || !ctx->have_window || ctx->window_aggregations == 0U)
        return;

    window_ns = (uint64_t)ctx->window_ms * 1000000ULL;
    trace_start_ns = ctx->warmup_end_ts_ns + ctx->current_window_bin * window_ns;
    trace_end_ns = trace_start_ns + window_ns;
    start_ms = (trace_start_ns - ctx->sync_trace_ts_ns) / 1000000ULL;
    end_ms = (trace_end_ns - ctx->sync_trace_ts_ns) / 1000000ULL;

    printf("WINDOW_TIME_META,%" PRIu64
           ",trace_start_ns=%" PRIu64 ",trace_end_ns=%" PRIu64
           ",sync_rel_start_ms=%" PRIu64 ",sync_rel_end_ms=%" PRIu64
           ",aggregations=%u,complete=%d\n",
           ctx->window_index,
           trace_start_ns, trace_end_ns,
           start_ms, end_ms,
           ctx->window_aggregations,
           complete ? 1 : 0);

    print_window_diag(ctx, start_ms, end_ms);
    g2_class_projector_print_window_report(ctx->projector,
                                           stdout,
                                           ctx->window_index,
                                           start_ms,
                                           end_ms,
                                           ctx->window_aggregations,
                                           complete);
    fflush(stdout);
    g2_class_projector_reset_window(ctx->projector);
    reset_window_diag(&ctx->window_diag);
    ++ctx->window_index;
    ctx->window_aggregations = 0U;
    ctx->have_window = 0;
}

static int ensure_agg_capacity(run_ctx_t *ctx, unsigned int expected)
{
    damon_region_sample_t *tmp;
    size_t newcap;

    if ((size_t)expected <= ctx->agg_capacity)
        return 0;
    newcap = ctx->agg_capacity != 0U ? ctx->agg_capacity : 256U;
    while (newcap < (size_t)expected) {
        if (newcap > SIZE_MAX / 2U) {
            errno = EOVERFLOW;
            return -1;
        }
        newcap *= 2U;
    }
    tmp = realloc(ctx->agg_samples, newcap * sizeof(*tmp));
    if (tmp == NULL)
        return -1;
    ctx->agg_samples = tmp;
    ctx->agg_capacity = newcap;
    return 0;
}

static void account_region_diag(run_ctx_t *ctx,
                                const damon_region_sample_t *s,
                                const g2_region_projection_stats_t *st)
{
    uint64_t ws_pages = 0U;
    if (ctx->have_workset)
        ws_pages = page_span_overlap(s->start, s->end,
                                     ctx->workset_start, ctx->workset_end,
                                     ctx->page_size);

    if (s->nr_accesses == 0U) {
        ++ctx->window_diag.cold_regions;
        ctx->window_diag.cold_present_pages += st->pages_present;
        ctx->window_diag.cold_workset_span_pages += ws_pages;
    } else if (s->nr_accesses >= ctx->hot_threshold) {
        ++ctx->window_diag.hot_regions;
        ctx->window_diag.hot_present_pages += st->pages_present;
        ctx->window_diag.hot_workset_span_pages += ws_pages;
    } else {
        ++ctx->window_diag.low_regions;
        ctx->window_diag.low_present_pages += st->pages_present;
        ctx->window_diag.low_workset_span_pages += ws_pages;
    }
}

static int project_complete_aggregation(run_ctx_t *ctx,
                                        uint64_t agg_ts_ns,
                                        uint64_t bin)
{
    size_t i;

    if (ctx->have_window && bin != ctx->current_window_bin) {
        if (bin < ctx->current_window_bin) {
            errno = ERANGE;
            return -1;
        }
        flush_time_window(ctx, 1);
    }
    if (!ctx->have_window) {
        ctx->have_window = 1;
        ctx->current_window_bin = bin;
    }

    for (i = 0U; i < ctx->agg_count; ++i) {
        g2_region_projection_stats_t st;
        const damon_region_sample_t *s = &ctx->agg_samples[i];
        uint64_t pstart = s->start;
        uint64_t pend = s->end;

        if (ctx->restrict_workset) {
            if (pstart < ctx->workset_start)
                pstart = ctx->workset_start;
            if (pend > ctx->workset_end)
                pend = ctx->workset_end;
            if (pend <= pstart)
                continue;
        }

        if (g2_class_projector_project_region_stats(ctx->projector,
                                                    ctx->pagemap_fd,
                                                    pstart,
                                                    pend,
                                                    s->nr_accesses,
                                                    &st) < 0) {
            return -1;
        }
        account_region_diag(ctx, s, &st);
    }

    ++ctx->window_aggregations;
    (void)agg_ts_ns;
    return 0;
}

static void commit_aggregation(run_ctx_t *ctx)
{
    uint64_t agg_ts_ns;
    uint64_t agg_last_trace_ns;
    uint64_t delta_us = 0U;
    uint64_t rel_sync_ms;
    uint64_t window_ns;
    uint64_t bin;
    uint64_t trace_emit_span_us = 0U;
    uint64_t callback_span_us = 0U;
    int64_t callback_minus_trace_rel_us = 0;
    uint64_t trace_rel_us = 0U;
    uint64_t callback_rel_us = 0U;

    if (ctx == NULL || ctx->agg_count == 0U)
        return;

    agg_ts_ns = ctx->agg_samples[0].trace_ts_ns;
    agg_last_trace_ns = ctx->agg_samples[ctx->agg_count - 1U].trace_ts_ns;
    if (agg_last_trace_ns >= agg_ts_ns)
        trace_emit_span_us = (agg_last_trace_ns - agg_ts_ns) / 1000ULL;
    if (ctx->agg_last_callback_ns >= ctx->agg_first_callback_ns)
        callback_span_us = (ctx->agg_last_callback_ns - ctx->agg_first_callback_ns) / 1000ULL;
    /*
     * Do not subtract absolute ftrace and CLOCK_MONOTONIC timestamps.
     * Their epochs/trace clocks are not guaranteed to be identical.  Compare
     * elapsed time since the same synchronization event instead.  Positive
     * values mean user-space callback processing is falling behind trace time.
     */
    if (agg_ts_ns >= ctx->sync_trace_ts_ns)
        trace_rel_us = (agg_ts_ns - ctx->sync_trace_ts_ns) / 1000ULL;
    if (ctx->agg_first_callback_ns >= ctx->sync_callback_ns)
        callback_rel_us = (ctx->agg_first_callback_ns - ctx->sync_callback_ns) / 1000ULL;
    callback_minus_trace_rel_us =
        (int64_t)callback_rel_us - (int64_t)trace_rel_us;
    if (ctx->previous_agg_ts_ns != 0U && agg_ts_ns >= ctx->previous_agg_ts_ns)
        delta_us = (agg_ts_ns - ctx->previous_agg_ts_ns) / 1000ULL;
    ctx->previous_agg_ts_ns = agg_ts_ns;
    rel_sync_ms = (agg_ts_ns - ctx->sync_trace_ts_ns) / 1000000ULL;

    printf("AGG_COMPLETE,%" PRIu64
           ",trace_ts_ns=%" PRIu64 ",sync_rel_ms=%" PRIu64
           ",delta_prev_us=%" PRIu64 ",regions=%zu"
           ",trace_emit_span_us=%" PRIu64
           ",callback_span_us=%" PRIu64
           ",trace_rel_us=%" PRIu64 ",callback_rel_us=%" PRIu64
           ",callback_minus_trace_rel_us=%" PRId64 "\n",
           ctx->completed_aggregations,
           agg_ts_ns,
           rel_sync_ms,
           delta_us,
           ctx->agg_count,
           trace_emit_span_us,
           callback_span_us,
           trace_rel_us,
           callback_rel_us,
           callback_minus_trace_rel_us);
    ++ctx->completed_aggregations;

    if (agg_ts_ns < ctx->warmup_end_ts_ns) {
        ++ctx->warmup_dropped_aggregations;
        printf("AGG_WARMUP_DROP,trace_ts_ns=%" PRIu64
               ",sync_rel_ms=%" PRIu64 ",regions=%zu\n",
               agg_ts_ns, rel_sync_ms, ctx->agg_count);
        return;
    }

    window_ns = (uint64_t)ctx->window_ms * 1000000ULL;
    bin = (agg_ts_ns - ctx->warmup_end_ts_ns) / window_ns;

    if (project_complete_aggregation(ctx, agg_ts_ns, bin) < 0) {
        fprintf(stderr, "[ERROR] project complete aggregation: %s\n",
                strerror(errno));
        ctx->projection_failed = 1;
        g_stop = 1;
    }
}

static void begin_or_append_synced(run_ctx_t *ctx,
                                   const damon_region_sample_t *s,
                                   uint64_t callback_ns)
{
    if (ctx->agg_count == 0U) {
        if (s->nr_regions == 0U || ensure_agg_capacity(ctx, s->nr_regions) < 0) {
            fprintf(stderr, "[ERROR] cannot start aggregation: %s\n", strerror(errno));
            ctx->boundary_failed = 1;
            g_stop = 1;
            return;
        }
        ctx->agg_expected = s->nr_regions;
        ctx->agg_first_callback_ns = callback_ns;
    } else if (s->nr_regions != ctx->agg_expected) {
        fprintf(stderr,
                "[ERROR] nr_regions changed inside aggregation: expected=%u got=%u count=%zu\n",
                ctx->agg_expected, s->nr_regions, ctx->agg_count);
        ctx->boundary_failed = 1;
        g_stop = 1;
        return;
    }

    if (ctx->agg_count >= ctx->agg_capacity) {
        fprintf(stderr, "[ERROR] aggregation buffer overflow\n");
        ctx->boundary_failed = 1;
        g_stop = 1;
        return;
    }

    ctx->agg_samples[ctx->agg_count++] = *s;
    ctx->agg_last_callback_ns = callback_ns;
    ctx->last_region_start = s->start;

    if (ctx->agg_count == (size_t)ctx->agg_expected) {
        commit_aggregation(ctx);
        ctx->agg_count = 0U;
        ctx->agg_expected = 0U;
    } else if (ctx->agg_count > (size_t)ctx->agg_expected) {
        fprintf(stderr, "[ERROR] aggregation count overflow\n");
        ctx->boundary_failed = 1;
        g_stop = 1;
    }
}

static void observe_sample(const damon_region_sample_t *s, void *user_data)
{
    run_ctx_t *ctx = user_data;
    uint64_t cb_ns = monotonic_ns();
    uint64_t cb_rel_ms = cb_ns >= ctx->callback_t0_ns
                             ? (cb_ns - ctx->callback_t0_ns) / 1000000ULL
                             : 0U;

    printf("REGION,callback_rel_ms=%" PRIu64
           ",callback_mono_ns=%" PRIu64
           ",trace_ts_ns=%" PRIu64 ",target_id=%u,pid=%d,nr_regions=%u"
           ",start=0x%llx,end=0x%llx,size_bytes=%llu,nr_accesses=%u,age=%u\n",
           cb_rel_ms,
           cb_ns,
           s->trace_ts_ns,
           s->target_id,
           (int)s->pid,
           s->nr_regions,
           (unsigned long long)s->start,
           (unsigned long long)s->end,
           (unsigned long long)(s->end - s->start),
           s->nr_accesses,
           s->age);

    if (!ctx->boundary_synced) {
        if (!ctx->have_last_region_start) {
            ctx->have_last_region_start = 1;
            ctx->last_region_start = s->start;
            ++ctx->dropped_initial_regions;
            return;
        }

        if (s->start <= ctx->last_region_start) {
            ctx->boundary_synced = 1;
            ctx->sync_trace_ts_ns = s->trace_ts_ns;
            ctx->sync_callback_ns = cb_ns;
            ctx->warmup_end_ts_ns = ctx->sync_trace_ts_ns +
                                    (uint64_t)ctx->warmup_ms * 1000000ULL;
            ctx->agg_count = 0U;
            ctx->agg_expected = 0U;
            printf("AGG_SYNC,trace_ts_ns=%" PRIu64
                   ",dropped_initial_regions=%" PRIu64
                   ",first_complete_expected=%u,first_start=0x%llx"
                   ",sync_callback_ns=%" PRIu64
                   ",warmup_end_ts_ns=%" PRIu64 "\n",
                   s->trace_ts_ns,
                   ctx->dropped_initial_regions,
                   s->nr_regions,
                   (unsigned long long)s->start,
                   ctx->sync_callback_ns,
                   ctx->warmup_end_ts_ns);
            begin_or_append_synced(ctx, s, cb_ns);
            return;
        }

        ctx->last_region_start = s->start;
        ++ctx->dropped_initial_regions;
        return;
    }

    /* A wrap before expected count means our count-based boundary was lost. */
    if (ctx->agg_count != 0U && s->start <= ctx->last_region_start) {
        fprintf(stderr,
                "[ERROR] new address wrap before expected aggregation count: "
                "seen=%zu expected=%u\n",
                ctx->agg_count, ctx->agg_expected);
        ctx->boundary_failed = 1;
        g_stop = 1;
        return;
    }

    begin_or_append_synced(ctx, s, cb_ns);
}

static int damon_admin_state_is_off(void)
{
    FILE *fp;
    char state[32];
    fp = fopen(DAMON_ADMIN_STATE_PATH, "re");
    if (fp == NULL)
        return -1;
    if (fscanf(fp, "%31s", state) != 1) {
        int saved = errno != 0 ? errno : EIO;
        fclose(fp);
        errno = saved;
        return -1;
    }
    fclose(fp);
    return strcmp(state, "off") == 0 ? 1 : 0;
}

static int stop_observer_allow_already_off(damon_observer_t *obs)
{
    int saved_errno;
    int state_off;
    if (damon_observer_stop(obs) == 0)
        return 0;
    saved_errno = errno;
    if (saved_errno != EPERM) {
        errno = saved_errno;
        return -1;
    }
    state_off = damon_admin_state_is_off();
    if (state_off == 1) {
        fprintf(stderr,
                "[INFO] damon_observer_stop returned EPERM, but DAMON state "
                "is already off; treating shutdown as successful\n");
        return 0;
    }
    errno = saved_errno;
    return -1;
}

static int parse_long(const char *s, long min_v, long max_v, long *out)
{
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v < min_v || v > max_v)
        return -1;
    *out = v;
    return 0;
}

static int parse_u64_any(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = (uint64_t)v;
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
        fprintf(stderr, "[ERROR] calibration_load_result(%s): %s\n",
                path, calibration_strerror(rc));
        return -1;
    }
    rc = dram_mapping_init_from_calibration(mapping, calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr, "[ERROR] dram_mapping_init_from_calibration: %s\n",
                dram_mapping_strerror(rc));
        return -1;
    }
    printf("# calibration=%s\n", path);
    printf("# mask_set_id=%s mask_count=%zu\n",
           mapping->mask_set_id, mapping->mask_count);
    for (i = 0U; i < mapping->mask_count; ++i)
        printf("# mask[%zu]=0x%016" PRIx64 "\n", i, mapping->masks[i]);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: sudo %s [--duration SEC] [--hot-threshold N] "
            "[--warmup-ms N] [--window-ms N] "
            "[--workset-start ADDR --workset-end ADDR] [--restrict-workset] "
            "[--calibration PATH] PID\n",
            prog);
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
    int duration_sec = 10;
    unsigned int hot_threshold = 10U;
    unsigned int warmup_ms = 1000U;
    unsigned int window_ms = 1000U;
    uint64_t workset_start = 0U, workset_end = 0U;
    int have_ws_start = 0, have_ws_end = 0;
    pid_t pid = -1;
    long page_size_l;
    int pagemap_fd = -1;
    char pagemap_path[128];
    uint64_t deadline_ns;
    uint64_t trace_lines, matched_lines, parse_fail_lines;
    int rc_main = 0;
    int i;

    memset(&projector, 0, sizeof(projector));
    memset(&ctx, 0, sizeof(ctx));
    damon_observer_config_default(&cfg);

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration") == 0 ||
            strcmp(argv[i], "--hot-threshold") == 0 ||
            strcmp(argv[i], "--warmup-ms") == 0 ||
            strcmp(argv[i], "--window-ms") == 0) {
            long v;
            const char *opt = argv[i];
            long min_v = strcmp(opt, "--warmup-ms") == 0 ? 0 : 1;
            long max_v = strcmp(opt, "--duration") == 0 ? 86400 : 1000000;
            if (++i >= argc || parse_long(argv[i], min_v, max_v, &v) != 0) {
                usage(argv[0]);
                return 2;
            }
            if (strcmp(opt, "--duration") == 0) duration_sec = (int)v;
            else if (strcmp(opt, "--hot-threshold") == 0) hot_threshold = (unsigned int)v;
            else if (strcmp(opt, "--warmup-ms") == 0) warmup_ms = (unsigned int)v;
            else window_ms = (unsigned int)v;
        } else if (strcmp(argv[i], "--workset-start") == 0 ||
                   strcmp(argv[i], "--workset-end") == 0) {
            uint64_t v;
            const char *opt = argv[i];
            if (++i >= argc || parse_u64_any(argv[i], &v) != 0) {
                usage(argv[0]);
                return 2;
            }
            if (strcmp(opt, "--workset-start") == 0) {
                workset_start = v; have_ws_start = 1;
            } else {
                workset_end = v; have_ws_end = 1;
            }
        } else if (strcmp(argv[i], "--restrict-workset") == 0) {
            ctx.restrict_workset = 1;
        } else if (strcmp(argv[i], "--calibration") == 0) {
            if (++i >= argc) { usage(argv[0]); return 2; }
            calibration_path = argv[i];
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            long v;
            if (pid > 0 || parse_long(argv[i], 1, 2147483647L, &v) != 0) {
                usage(argv[0]);
                return 2;
            }
            pid = (pid_t)v;
        }
    }

    if (pid <= 0 || have_ws_start != have_ws_end ||
        (have_ws_start && workset_end <= workset_start)) {
        usage(argv[0]);
        return 2;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "run as root: DAMON and pagemap PFN access require privilege\n");
        return 1;
    }
    if (load_mapping(calibration_path, &calibration, &mapping) < 0)
        return 1;

    page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return 1;
    }
    if (g2_class_projector_init(&projector, &mapping,
                                (size_t)page_size_l, hot_threshold) < 0) {
        perror("g2_class_projector_init");
        return 1;
    }

    if (snprintf(pagemap_path, sizeof(pagemap_path),
                 "/proc/%d/pagemap", (int)pid) >= (int)sizeof(pagemap_path)) {
        fprintf(stderr, "pagemap path too long\n");
        rc_main = 1; goto out_projector;
    }
    pagemap_fd = open(pagemap_path, O_RDONLY | O_CLOEXEC);
    if (pagemap_fd < 0) {
        perror("open(pagemap)");
        rc_main = 1; goto out_projector;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (damon_observer_init(&obs, &cfg, &pid, 1U) < 0) {
        perror("damon_observer_init");
        rc_main = 1; goto out_fd;
    }
    if (damon_observer_start(&obs) < 0) {
        perror("damon_observer_start");
        rc_main = 1; damon_observer_destroy(&obs); goto out_fd;
    }

    ctx.callback_t0_ns = monotonic_ns();
    ctx.projector = &projector;
    ctx.pagemap_fd = pagemap_fd;
    ctx.page_size = (size_t)page_size_l;
    ctx.hot_threshold = hot_threshold;
    ctx.warmup_ms = warmup_ms;
    ctx.window_ms = window_ms;
    ctx.have_workset = have_ws_start;
    if (ctx.restrict_workset && !ctx.have_workset) {
        fprintf(stderr, "--restrict-workset requires --workset-start/--workset-end\n");
        rc_main = 2;
        (void)stop_observer_allow_already_off(&obs);
        damon_observer_destroy(&obs);
        goto out_fd;
    }
    ctx.workset_start = workset_start;
    ctx.workset_end = workset_end;
    reset_window_diag(&ctx.window_diag);

    printf("# Stage2A timestamp-based observer\n");
    printf("# trace_clock=%s previous_trace_clock=%s changed=%d\n",
           obs.trace_clock_name[0] ? obs.trace_clock_name : "unknown",
           obs.trace_clock_previous[0] ? obs.trace_clock_previous : "unknown",
           obs.trace_clock_changed);
    printf("# pid=%d duration_sec=%d sample_us=%lu aggr_us=%lu update_us=%lu "
           "min_regions=%u max_regions=%u\n",
           (int)pid, duration_sec, cfg.sample_us, cfg.aggr_us, cfg.update_us,
           cfg.min_regions, cfg.max_regions);
    printf("# window_mode=kernel_trace_timestamp window_ms=%u warmup_ms=%u\n",
           window_ms, warmup_ms);
    if (ctx.have_workset)
        printf("# workset=0x%" PRIx64 "-0x%" PRIx64 " restrict=%d\n",
               ctx.workset_start, ctx.workset_end, ctx.restrict_workset);
    printf("# initial_partial=drop trailing_partial=buffer_and_drop\n");
    fflush(stdout);

    deadline_ns = ctx.callback_t0_ns + (uint64_t)duration_sec * 1000000000ULL;
    while (!g_stop) {
        uint64_t now_ns = monotonic_ns();
        int prc;
        if (now_ns != 0U && now_ns >= deadline_ns)
            break;
        prc = damon_observer_poll(&obs, 250, observe_sample, &ctx);
        if (prc < 0) {
            perror("damon_observer_poll");
            rc_main = 1;
            break;
        }
    }

    if (stop_observer_allow_already_off(&obs) < 0) {
        perror("damon_observer_stop");
        rc_main = 1;
    }

    if (ctx.agg_count > 0U) {
        fprintf(stderr,
                "[WARN] dropping trailing incomplete aggregation: seen=%zu expected=%u\n",
                ctx.agg_count, ctx.agg_expected);
    }
    if (ctx.have_window && ctx.window_aggregations > 0U)
        flush_time_window(&ctx, 0);

    damon_observer_get_parse_stats(&obs, &trace_lines, &matched_lines, &parse_fail_lines);
    printf("# OBSERVER_PARSE_STATS trace_lines=%" PRIu64
           " matched=%" PRIu64 " parse_fail=%" PRIu64 "\n",
           trace_lines, matched_lines, parse_fail_lines);
    printf("# TIME_BOUNDARY_SUMMARY synced=%d dropped_initial=%" PRIu64
           " completed=%" PRIu64 " warmup_dropped=%" PRIu64
           " trailing_seen=%zu trailing_expected=%u\n",
           ctx.boundary_synced,
           ctx.dropped_initial_regions,
           ctx.completed_aggregations,
           ctx.warmup_dropped_aggregations,
           ctx.agg_count,
           ctx.agg_expected);
    g2_class_projector_print_report(&projector, stdout);
    fflush(stdout);

    if (ctx.projection_failed || ctx.boundary_failed || parse_fail_lines != 0U)
        rc_main = 1;

    damon_observer_destroy(&obs);
    free(ctx.agg_samples);

out_fd:
    if (pagemap_fd >= 0)
        close(pagemap_fd);
out_projector:
    g2_class_projector_destroy(&projector);
    return rc_main;
}

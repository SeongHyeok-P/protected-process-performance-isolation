#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MIB (1024ULL * 1024ULL)

typedef struct {
    volatile uint8_t *start;
    size_t len;
    size_t page_size;
    unsigned int period_ms;
    const char *name;
    atomic_ullong sweeps;
    atomic_ullong checksum;
} worker_region_t;

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Controlled DAMON validation workload.\n"
        "\n"
        "Options:\n"
        "  --hot-mib N          HOT region size in MiB (default: 8)\n"
        "  --warm-mib N         WARM region size in MiB (default: 8)\n"
        "  --cold-mib N         COLD region size in MiB (default: 240)\n"
        "  --warm-period-ms N   WARM full sweep period in ms (default: 50)\n"
        "  --duration SEC       Run duration; 0 means until signal (default: 60)\n"
        "  --status-sec SEC     Status print interval (default: 5)\n"
        "  --help               Show this help\n"
        "\n"
        "Access pattern:\n"
        "  HOT  : continuously touches one byte in every 4 KiB page\n"
        "  WARM : touches one byte in every 4 KiB page once per period\n"
        "  COLD : pages are faulted once during setup, then never touched\n",
        prog);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s || !out)
        return -1;

    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || !end || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int sleep_ms(unsigned int ms)
{
    struct timespec req;

    req.tv_sec = (time_t)(ms / 1000U);
    req.tv_nsec = (long)(ms % 1000U) * 1000000L;

    while (nanosleep(&req, &req) < 0) {
        if (errno != EINTR)
            return -1;
        if (g_stop)
            return 0;
    }
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void fault_region(volatile uint8_t *p, size_t len, size_t page_size,
                         uint8_t seed)
{
    size_t off;

    for (off = 0; off < len; off += page_size)
        p[off] = (uint8_t)(seed + (uint8_t)(off / page_size));
}

static void *hot_worker(void *arg)
{
    worker_region_t *r = arg;
    unsigned long long local_sum = 0;

    while (!g_stop) {
        size_t off;

        for (off = 0; off < r->len; off += r->page_size)
            local_sum += r->start[off];

        atomic_fetch_add_explicit(&r->sweeps, 1, memory_order_relaxed);
    }

    atomic_store_explicit(&r->checksum, local_sum, memory_order_relaxed);
    return NULL;
}

static void *warm_worker(void *arg)
{
    worker_region_t *r = arg;
    unsigned long long local_sum = 0;

    while (!g_stop) {
        double t0 = monotonic_seconds();
        double elapsed;
        size_t off;
        unsigned int sleep_for;

        for (off = 0; off < r->len; off += r->page_size)
            local_sum += r->start[off];

        atomic_fetch_add_explicit(&r->sweeps, 1, memory_order_relaxed);

        elapsed = monotonic_seconds() - t0;
        if (elapsed < 0.0)
            elapsed = 0.0;

        if (elapsed * 1000.0 >= (double)r->period_ms)
            continue;

        sleep_for = r->period_ms - (unsigned int)(elapsed * 1000.0);
        if (sleep_ms(sleep_for) < 0)
            break;
    }

    atomic_store_explicit(&r->checksum, local_sum, memory_order_relaxed);
    return NULL;
}

static void print_region(const char *name, volatile uint8_t *start, size_t len,
                         const char *pattern)
{
    uintptr_t s = (uintptr_t)start;
    uintptr_t e = s + len;

    printf("REGION,%s,0x%" PRIxPTR ",0x%" PRIxPTR ",%zu,%s\n",
           name, s, e, len, pattern);
}

int main(int argc, char **argv)
{
    uint64_t hot_mib = 8;
    uint64_t warm_mib = 8;
    uint64_t cold_mib = 240;
    uint64_t warm_period_ms = 50;
    uint64_t duration_sec = 60;
    uint64_t status_sec = 5;
    long ps;
    size_t page_size;
    size_t hot_len, warm_len, cold_len;
    size_t total_len;
    uint8_t *base;
    uint8_t *hot;
    uint8_t *warm;
    uint8_t *cold;
    pthread_t hot_tid, warm_tid;
    worker_region_t hot_region, warm_region;
    bool hot_started = false;
    bool warm_started = false;
    double start_time, next_status;
    int i;

    for (i = 1; i < argc; i++) {
        uint64_t v;

        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }

#define PARSE_OPT(name, dst) \
        if (strcmp(argv[i], name) == 0) { \
            if (++i >= argc || parse_u64(argv[i], &v) < 0) { \
                fprintf(stderr, "invalid value for %s\n", name); \
                return 2; \
            } \
            dst = v; \
            continue; \
        }

        PARSE_OPT("--hot-mib", hot_mib)
        PARSE_OPT("--warm-mib", warm_mib)
        PARSE_OPT("--cold-mib", cold_mib)
        PARSE_OPT("--warm-period-ms", warm_period_ms)
        PARSE_OPT("--duration", duration_sec)
        PARSE_OPT("--status-sec", status_sec)
#undef PARSE_OPT

        fprintf(stderr, "unknown option: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    if (hot_mib == 0 || warm_mib == 0 || cold_mib == 0 ||
        warm_period_ms == 0 || warm_period_ms > 3600000ULL ||
        status_sec == 0 || status_sec > 86400ULL) {
        fprintf(stderr,
                "invalid size/period: sizes > 0, warm period <= 3600000 ms, "
                "status <= 86400 s\n");
        return 2;
    }

    if (hot_mib > SIZE_MAX / MIB || warm_mib > SIZE_MAX / MIB ||
        cold_mib > SIZE_MAX / MIB) {
        fprintf(stderr, "region size too large\n");
        return 2;
    }

    ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return 1;
    }
    page_size = (size_t)ps;

    hot_len = (size_t)(hot_mib * MIB);
    warm_len = (size_t)(warm_mib * MIB);
    cold_len = (size_t)(cold_mib * MIB);

    if (hot_len > SIZE_MAX - warm_len ||
        hot_len + warm_len > SIZE_MAX - cold_len ||
        hot_len + warm_len + cold_len > SIZE_MAX - 2 * page_size) {
        fprintf(stderr, "total mapping size overflow\n");
        return 2;
    }

    total_len = hot_len + warm_len + cold_len + 2 * page_size;

    base = mmap(NULL, total_len, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap reserve");
        return 1;
    }

    hot = base;
    warm = hot + hot_len + page_size;
    cold = warm + warm_len + page_size;

    if (mprotect(hot, hot_len, PROT_READ | PROT_WRITE) < 0 ||
        mprotect(warm, warm_len, PROT_READ | PROT_WRITE) < 0 ||
        mprotect(cold, cold_len, PROT_READ | PROT_WRITE) < 0) {
        perror("mprotect");
        munmap(base, total_len);
        return 1;
    }

#ifdef MADV_NOHUGEPAGE
    (void)madvise(hot, hot_len, MADV_NOHUGEPAGE);
    (void)madvise(warm, warm_len, MADV_NOHUGEPAGE);
    (void)madvise(cold, cold_len, MADV_NOHUGEPAGE);
#endif

    /* Fault every page exactly once before the controlled access phase. */
    fault_region(hot, hot_len, page_size, 0x11);
    fault_region(warm, warm_len, page_size, 0x22);
    fault_region(cold, cold_len, page_size, 0x33);

    memset(&hot_region, 0, sizeof(hot_region));
    hot_region.start = hot;
    hot_region.len = hot_len;
    hot_region.page_size = page_size;
    hot_region.name = "HOT";
    atomic_init(&hot_region.sweeps, 0);
    atomic_init(&hot_region.checksum, 0);

    memset(&warm_region, 0, sizeof(warm_region));
    warm_region.start = warm;
    warm_region.len = warm_len;
    warm_region.page_size = page_size;
    warm_region.period_ms = (unsigned int)warm_period_ms;
    warm_region.name = "WARM";
    atomic_init(&warm_region.sweeps, 0);
    atomic_init(&warm_region.checksum, 0);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("# Stage7-A controlled DAMON validation workload\n");
    printf("# pid=%d page_size=%zu total_mib=%" PRIu64
           " hot_mib=%" PRIu64 " warm_mib=%" PRIu64
           " cold_mib=%" PRIu64 " warm_period_ms=%" PRIu64 "\n",
           (int)getpid(), page_size, hot_mib + warm_mib + cold_mib,
           hot_mib, warm_mib, cold_mib, warm_period_ms);
    printf("TYPE,name,start,end,size_bytes,pattern\n");
    print_region("HOT", hot, hot_len, "continuous_page_sweep");
    print_region("WARM", warm, warm_len, "periodic_page_sweep");
    print_region("COLD", cold, cold_len, "setup_fault_only_no_runtime_access");
    printf("READY,pid=%d\n", (int)getpid());
    fflush(stdout);

    if (pthread_create(&hot_tid, NULL, hot_worker, &hot_region) != 0) {
        fprintf(stderr, "pthread_create(HOT) failed\n");
        munmap(base, total_len);
        return 1;
    }
    hot_started = true;

    if (pthread_create(&warm_tid, NULL, warm_worker, &warm_region) != 0) {
        fprintf(stderr, "pthread_create(WARM) failed\n");
        g_stop = 1;
        pthread_join(hot_tid, NULL);
        munmap(base, total_len);
        return 1;
    }
    warm_started = true;

    start_time = monotonic_seconds();
    next_status = start_time + (double)status_sec;

    while (!g_stop) {
        double now = monotonic_seconds();

        if (duration_sec > 0 && now - start_time >= (double)duration_sec)
            break;

        if (now >= next_status) {
            printf("STATUS,elapsed=%.3f,hot_sweeps=%llu,warm_sweeps=%llu\n",
                   now - start_time,
                   (unsigned long long)atomic_load_explicit(&hot_region.sweeps,
                                                            memory_order_relaxed),
                   (unsigned long long)atomic_load_explicit(&warm_region.sweeps,
                                                            memory_order_relaxed));
            fflush(stdout);
            next_status += (double)status_sec;
        }

        if (sleep_ms(100) < 0)
            break;
    }

    g_stop = 1;
    if (hot_started)
        pthread_join(hot_tid, NULL);
    if (warm_started)
        pthread_join(warm_tid, NULL);

    printf("DONE,hot_sweeps=%llu,warm_sweeps=%llu,hot_checksum=%llu,warm_checksum=%llu\n",
           (unsigned long long)atomic_load_explicit(&hot_region.sweeps,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&warm_region.sweeps,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&hot_region.checksum,
                                                    memory_order_relaxed),
           (unsigned long long)atomic_load_explicit(&warm_region.checksum,
                                                    memory_order_relaxed));
    fflush(stdout);

    if (munmap(base, total_len) < 0) {
        perror("munmap");
        return 1;
    }
    return 0;
}


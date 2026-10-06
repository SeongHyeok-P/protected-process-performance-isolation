#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CALIBRATION_PATH "/run/protected-daemon/dram-map.json"
#define CACHELINE_BYTES 64U
#define MIB (1024ULL * 1024ULL)
#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_SWAPPED (1ULL << 62)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1ULL)
#define MAX_GROUPS 32U
#define MAX_STREAMS 32U
#define CHUNK_ITERS 1024U

static volatile sig_atomic_t g_stop;
static volatile uintptr_t g_sink;

struct cfg {
    const char *name;
    const char *role;
    size_t pool_mib;
    unsigned int hot_pages_total;
    unsigned int duration_sec;
    unsigned int streams;
    unsigned int pace_us;
    int cpu;
    uint64_t seed;
    const char *calibration_path;
    unsigned int groups[MAX_GROUPS];
    size_t nr_groups;
};

struct selected_page {
    unsigned char *addr;
    uint64_t pfn;
    unsigned int group;
};

static void on_stop(int signo)
{
    (void)signo;
    g_stop = 1;
}

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0U;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --name NAME --role victim|aggressor --groups LIST [options]\n"
        "  --pool-mib N          default 256\n"
        "  --hot-pages-total N   default 16384 (64 MiB of pages)\n"
        "  --duration SEC        default 20\n"
        "  --streams N           aggressor default 16, victim forced 1\n"
        "  --pace-us N           sleep N us after each 1024-iteration chunk (default 0)\n"
        "  --cpu N               optional affinity\n"
        "  --seed N              shuffle seed\n"
        "  --calibration PATH    default /run/protected-daemon/dram-map.json\n",
        prog);
}

static int parse_ul(const char *s, unsigned long minv, unsigned long maxv,
                    unsigned long *out)
{
    char *end = NULL;
    unsigned long v;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < minv || v > maxv)
        return -1;
    *out = v;
    return 0;
}

static int parse_u64(const char *s, uint64_t *out)
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

static int parse_groups(const char *text, struct cfg *cfg)
{
    char *copy = NULL;
    char *save = NULL;
    char *tok;
    size_t n = 0U;

    copy = strdup(text);
    if (copy == NULL)
        return -1;

    for (tok = strtok_r(copy, ",", &save);
         tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        unsigned long v;
        size_t i;
        if (n >= MAX_GROUPS || parse_ul(tok, 0UL, 31UL, &v) != 0) {
            free(copy);
            return -1;
        }
        for (i = 0U; i < n; ++i) {
            if (cfg->groups[i] == (unsigned int)v) {
                free(copy);
                return -1;
            }
        }
        cfg->groups[n++] = (unsigned int)v;
    }
    free(copy);

    if (n == 0U)
        return -1;
    cfg->nr_groups = n;
    return 0;
}

static int pin_cpu(int cpu)
{
    cpu_set_t set;
    if (cpu < 0)
        return 0;
    CPU_ZERO(&set);
    CPU_SET((unsigned int)cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static int read_pfn(int fd, uintptr_t va, size_t page_size, uint64_t *pfn_out)
{
    uint64_t entry;
    off_t off = (off_t)((va / page_size) * sizeof(entry));
    ssize_t n = pread(fd, &entry, sizeof(entry), off);
    if (n != (ssize_t)sizeof(entry))
        return -1;
    if ((entry & PAGEMAP_PRESENT) == 0U || (entry & PAGEMAP_SWAPPED) != 0U)
        return -1;
    *pfn_out = entry & PAGEMAP_PFN_MASK;
    return *pfn_out == 0U ? -1 : 0;
}

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t aa = *(const uint64_t *)a;
    const uint64_t bb = *(const uint64_t *)b;
    return (aa > bb) - (aa < bb);
}

static int build_dense_reps(const struct dram_mapping *mapping,
                            const uint64_t *deltas,
                            size_t delta_count,
                            uint64_t **reps_out,
                            size_t *count_out)
{
    size_t raw_count;
    uint64_t *tmp = NULL;
    uint64_t *reps = NULL;
    size_t i;
    size_t n = 0U;

    if (mapping == NULL || reps_out == NULL || count_out == NULL ||
        mapping->mask_count >= 20U)
        return -1;

    raw_count = (size_t)1ULL << mapping->mask_count;
    tmp = calloc(raw_count, sizeof(*tmp));
    if (tmp == NULL)
        return -1;

    for (i = 0U; i < raw_count; ++i)
        tmp[i] = dram_mapping_canonical_group_fast((uint64_t)i,
                                                    deltas, delta_count);
    qsort(tmp, raw_count, sizeof(*tmp), cmp_u64);
    for (i = 0U; i < raw_count; ++i) {
        if (i == 0U || tmp[i] != tmp[i - 1U])
            ++n;
    }

    reps = calloc(n, sizeof(*reps));
    if (reps == NULL) {
        free(tmp);
        return -1;
    }

    n = 0U;
    for (i = 0U; i < raw_count; ++i) {
        if (i == 0U || tmp[i] != tmp[i - 1U])
            reps[n++] = tmp[i];
    }
    free(tmp);
    *reps_out = reps;
    *count_out = n;
    return 0;
}

static int dense_group_for_pfn(const struct dram_mapping *mapping,
                               const uint64_t *deltas,
                               size_t delta_count,
                               const uint64_t *reps,
                               size_t rep_count,
                               uint64_t pfn,
                               size_t page_size,
                               unsigned int *group_out)
{
    const uint64_t pa = pfn * (uint64_t)page_size;
    const uint64_t raw = dram_mapping_bank_class_fast(mapping, pa);
    const uint64_t rep = dram_mapping_canonical_group_fast(raw,
                                                           deltas, delta_count);
    size_t lo = 0U;
    size_t hi = rep_count;

    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2U;
        if (reps[mid] < rep)
            lo = mid + 1U;
        else
            hi = mid;
    }
    if (lo >= rep_count || reps[lo] != rep)
        return -1;
    *group_out = (unsigned int)lo;
    return 0;
}

static int selected_slot(const struct cfg *cfg, unsigned int group)
{
    size_t i;
    for (i = 0U; i < cfg->nr_groups; ++i) {
        if (cfg->groups[i] == group)
            return (int)i;
    }
    return -1;
}

static uint64_t rng_next(uint64_t *state)
{
    uint64_t x = *state;
    if (x == 0U)
        x = 0x9e3779b97f4a7c15ULL;
    x ^= x >> 12U;
    x ^= x << 25U;
    x ^= x >> 27U;
    *state = x;
    return x * 2685821657736338717ULL;
}

static void shuffle_uintptr(uintptr_t *a, size_t n, uint64_t seed)
{
    uint64_t state = seed;
    size_t i;
    if (a == NULL || n < 2U)
        return;
    for (i = n - 1U; i > 0U; --i) {
        const size_t j = (size_t)(rng_next(&state) % (uint64_t)(i + 1U));
        const uintptr_t t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static int build_chains(uintptr_t *addresses,
                        size_t count,
                        unsigned int streams,
                        uintptr_t roots[MAX_STREAMS])
{
    unsigned int s;
    size_t base = 0U;

    if (addresses == NULL || roots == NULL || streams == 0U ||
        streams > MAX_STREAMS || count < streams)
        return -1;

    for (s = 0U; s < streams; ++s) {
        const size_t remain = count - base;
        const size_t stream_left = (size_t)streams - (size_t)s;
        const size_t len = remain / stream_left;
        size_t j;

        if (len == 0U)
            return -1;
        roots[s] = addresses[base];
        for (j = 0U; j < len; ++j) {
            const uintptr_t cur = addresses[base + j];
            const uintptr_t next = addresses[base + ((j + 1U) % len)];
            memcpy((void *)cur, &next, sizeof(next));
        }
        base += len;
    }
    return base == count ? 0 : -1;
}

static int prepare_gate(sigset_t *gate_set)
{
    if (gate_set == NULL)
        return -1;
    if (sigemptyset(gate_set) != 0 ||
        sigaddset(gate_set, SIGUSR1) != 0 ||
        sigprocmask(SIG_BLOCK, gate_set, NULL) != 0)
        return -1;
    return 0;
}

static int wait_for_gate(const sigset_t *gate_set)
{
    int sig = 0;
    if (gate_set == NULL)
        return -1;
    if (sigwait(gate_set, &sig) != 0 || sig != SIGUSR1)
        return -1;
    return 0;
}

static int pace_sleep_us(unsigned int usec)
{
    struct timespec req;
    struct timespec rem;

    if (usec == 0U)
        return 0;

    req.tv_sec = (time_t)(usec / 1000000U);
    req.tv_nsec = (long)(usec % 1000000U) * 1000L;

    while (!g_stop && nanosleep(&req, &rem) != 0) {
        if (errno != EINTR)
            return -1;
        req = rem;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct cfg cfg = {
        .name = "unnamed",
        .role = "aggressor",
        .pool_mib = 256U,
        .hot_pages_total = 16384U,
        .duration_sec = 20U,
        .streams = 16U,
        .pace_us = 0U,
        .cpu = -1,
        .seed = 20260902ULL,
        .calibration_path = DEFAULT_CALIBRATION_PATH,
        .nr_groups = 0U,
    };
    struct calibration_result cal;
    struct dram_mapping mapping;
    uint64_t deltas[DRAM_MAPPING_MAX_PAGE_DELTAS];
    size_t delta_count = 0U;
    uint64_t *reps = NULL;
    size_t rep_count = 0U;
    unsigned long ul;
    long page_size_l;
    size_t page_size = 0U;
    size_t pool_bytes = 0U;
    size_t page_count = 0U;
    unsigned char *pool = MAP_FAILED;
    int pagemap_fd = -1;
    uint64_t *page_pfn = NULL;
    uint64_t *pool_group_counts = NULL;
    unsigned int *filled = NULL;
    struct selected_page *selected = NULL;
    uintptr_t *addresses = NULL;
    uintptr_t roots[MAX_STREAMS] = {0U};
    size_t selected_total;
    size_t pages_per_group;
    size_t lines_per_page;
    size_t line_count;
    size_t selected_n = 0U;
    size_t i;
    int rc = EXIT_FAILURE;
    uint64_t start_ns;
    uint64_t deadline_ns;
    uint64_t operations = 0U;
    sigset_t gate_set;

    for (i = 1U; i < (size_t)argc; ++i) {
        if (strcmp(argv[i], "--name") == 0) {
            if (++i >= (size_t)argc) { usage(argv[0]); return 2; }
            cfg.name = argv[i];
        } else if (strcmp(argv[i], "--role") == 0) {
            if (++i >= (size_t)argc) { usage(argv[0]); return 2; }
            cfg.role = argv[i];
        } else if (strcmp(argv[i], "--groups") == 0) {
            if (++i >= (size_t)argc || parse_groups(argv[i], &cfg) != 0) {
                usage(argv[0]); return 2;
            }
        } else if (strcmp(argv[i], "--pool-mib") == 0) {
            if (++i >= (size_t)argc || parse_ul(argv[i], 64UL, 4096UL, &ul) != 0) {
                usage(argv[0]); return 2;
            }
            cfg.pool_mib = (size_t)ul;
        } else if (strcmp(argv[i], "--hot-pages-total") == 0) {
            if (++i >= (size_t)argc || parse_ul(argv[i], 1UL, 2000000UL, &ul) != 0) {
                usage(argv[0]); return 2;
            }
            cfg.hot_pages_total = (unsigned int)ul;
        } else if (strcmp(argv[i], "--duration") == 0) {
            if (++i >= (size_t)argc || parse_ul(argv[i], 1UL, 86400UL, &ul) != 0) {
                usage(argv[0]); return 2;
            }
            cfg.duration_sec = (unsigned int)ul;
        } else if (strcmp(argv[i], "--streams") == 0) {
            if (++i >= (size_t)argc || parse_ul(argv[i], 1UL, MAX_STREAMS, &ul) != 0) {
                usage(argv[0]); return 2;
            }
            cfg.streams = (unsigned int)ul;
        } else if (strcmp(argv[i], "--pace-us") == 0) {
            if (++i >= (size_t)argc || parse_ul(argv[i], 0UL, 1000000UL, &ul) != 0) {
                usage(argv[0]); return 2;
            }
            cfg.pace_us = (unsigned int)ul;
        } else if (strcmp(argv[i], "--cpu") == 0) {
            if (++i >= (size_t)argc || parse_ul(argv[i], 0UL, 4095UL, &ul) != 0) {
                usage(argv[0]); return 2;
            }
            cfg.cpu = (int)ul;
        } else if (strcmp(argv[i], "--seed") == 0) {
            if (++i >= (size_t)argc || parse_u64(argv[i], &cfg.seed) != 0) {
                usage(argv[0]); return 2;
            }
        } else if (strcmp(argv[i], "--calibration") == 0) {
            if (++i >= (size_t)argc) { usage(argv[0]); return 2; }
            cfg.calibration_path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.nr_groups == 0U ||
        (strcmp(cfg.role, "victim") != 0 && strcmp(cfg.role, "aggressor") != 0)) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(cfg.role, "victim") == 0) {
        cfg.streams = 1U;
        cfg.pace_us = 0U;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "[ERROR] run as root: pagemap PFN access is required\n");
        return 1;
    }

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0)
        goto out;
    page_size = (size_t)page_size_l;
    if (page_size % CACHELINE_BYTES != 0U)
        goto out;
    lines_per_page = page_size / CACHELINE_BYTES;

    pool_bytes = cfg.pool_mib * (size_t)MIB;
    pool_bytes = (pool_bytes / page_size) * page_size;
    page_count = pool_bytes / page_size;
    selected_total = (size_t)cfg.hot_pages_total;

    if (selected_total == 0U || selected_total % cfg.nr_groups != 0U ||
        selected_total >= page_count) {
        fprintf(stderr, "[ERROR] hot pages must be divisible by group count and smaller than pool\n");
        goto out;
    }
    pages_per_group = selected_total / cfg.nr_groups;
    if (selected_total > SIZE_MAX / lines_per_page)
        goto out;
    line_count = selected_total * lines_per_page;

    calibration_result_reset(&cal);
    dram_mapping_reset(&mapping);
    if (calibration_load_result(cfg.calibration_path, &cal) != CALIBRATION_OK) {
        fprintf(stderr, "[ERROR] calibration load failed: %s\n", cfg.calibration_path);
        goto out;
    }
    if (dram_mapping_init_from_calibration(&mapping, &cal) != DRAM_MAPPING_OK)
        goto out;
    if (dram_mapping_page_class_deltas(&mapping, page_size, CACHELINE_BYTES,
                                       deltas, NULL,
                                       DRAM_MAPPING_MAX_PAGE_DELTAS,
                                       &delta_count) != DRAM_MAPPING_OK ||
        !dram_mapping_page_deltas_form_xor_subgroup(deltas, delta_count)) {
        fprintf(stderr, "[ERROR] invalid page delta subgroup\n");
        goto out;
    }
    if (build_dense_reps(&mapping, deltas, delta_count, &reps, &rep_count) != 0)
        goto out;
    if (rep_count != 32U) {
        fprintf(stderr, "[ERROR] expected 32 groups, got %zu\n", rep_count);
        goto out;
    }
    for (i = 0U; i < cfg.nr_groups; ++i) {
        if (cfg.groups[i] >= rep_count) {
            fprintf(stderr, "[ERROR] group %u >= %zu\n", cfg.groups[i], rep_count);
            goto out;
        }
    }

    pool = mmap(NULL, pool_bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED)
        goto out;
    (void)madvise(pool, pool_bytes, MADV_NOHUGEPAGE);

    for (i = 0U; i < page_count; ++i)
        pool[i * page_size] = (unsigned char)(i * 29U + 7U);

    pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap_fd < 0)
        goto out;

    page_pfn = calloc(page_count, sizeof(*page_pfn));
    pool_group_counts = calloc(rep_count, sizeof(*pool_group_counts));
    filled = calloc(cfg.nr_groups, sizeof(*filled));
    selected = calloc(selected_total, sizeof(*selected));
    addresses = calloc(line_count, sizeof(*addresses));
    if (page_pfn == NULL || pool_group_counts == NULL || filled == NULL ||
        selected == NULL || addresses == NULL)
        goto out;

    for (i = 0U; i < page_count; ++i) {
        uint64_t pfn;
        unsigned int group;
        int slot;
        if (read_pfn(pagemap_fd, (uintptr_t)(pool + i * page_size),
                     page_size, &pfn) != 0) {
            fprintf(stderr, "[ERROR] unreadable PFN page=%zu\n", i);
            goto out;
        }
        if (dense_group_for_pfn(&mapping, deltas, delta_count,
                                reps, rep_count, pfn, page_size, &group) != 0)
            goto out;
        page_pfn[i] = pfn;
        ++pool_group_counts[group];
        slot = selected_slot(&cfg, group);
        if (slot >= 0 && filled[slot] < pages_per_group) {
            const size_t idx = (size_t)slot * pages_per_group + filled[slot];
            selected[idx].addr = pool + i * page_size;
            selected[idx].pfn = pfn;
            selected[idx].group = group;
            ++filled[slot];
            ++selected_n;
        }
    }

    if (selected_n != selected_total) {
        fprintf(stderr, "[ERROR] insufficient selected pages: got=%zu need=%zu\n",
                selected_n, selected_total);
        for (i = 0U; i < cfg.nr_groups; ++i)
            fprintf(stderr, "GROUP_CAPACITY,%u,%u,%zu\n",
                    cfg.groups[i], filled[i], pages_per_group);
        goto out;
    }

    for (i = 0U; i < rep_count; ++i)
        printf("POOL_GROUP,%zu,%" PRIu64 "\n", i, pool_group_counts[i]);

    for (i = 0U; i < cfg.nr_groups; ++i)
        printf("GROUND_TRUTH_GROUP,%u,0x%" PRIx64 ",%zu\n",
               cfg.groups[i], reps[cfg.groups[i]], pages_per_group);

    /* Expand every selected 4 KiB page into all of its 64 cache lines. */
    {
        size_t pos = 0U;
        size_t pidx;
        for (pidx = 0U; pidx < selected_total; ++pidx) {
            size_t line;
            for (line = 0U; line < lines_per_page; ++line)
                addresses[pos++] = (uintptr_t)(selected[pidx].addr +
                                                line * CACHELINE_BYTES);
        }
        if (pos != line_count)
            goto out;
    }

    shuffle_uintptr(addresses, line_count,
                    cfg.seed ^ 0xd1b54a32d192ed03ULL);
    if (build_chains(addresses, line_count, cfg.streams, roots) != 0) {
        fprintf(stderr, "[ERROR] chain construction failed\n");
        goto out;
    }

    if (pin_cpu(cfg.cpu) != 0) {
        perror("sched_setaffinity");
        goto out;
    }

    /* Block SIGUSR1 before READY so an early gate signal cannot terminate us. */
    if (prepare_gate(&gate_set) != 0) {
        perror("SIGUSR1 gate setup");
        goto out;
    }

    fflush(stdout);
    fprintf(stderr,
            "READY pid=%d name=%s role=%s pool_start=0x%016" PRIxPTR
            " pool_end=0x%016" PRIxPTR
            " pool_mib=%zu selected_pages=%zu selected_bytes=%zu"
            " lines=%zu streams=%u pace_us=%u groups=",
            (int)getpid(), cfg.name, cfg.role,
            (uintptr_t)pool, (uintptr_t)(pool + pool_bytes),
            cfg.pool_mib, selected_total, selected_total * page_size,
            line_count, cfg.streams, cfg.pace_us);
    for (i = 0U; i < cfg.nr_groups; ++i)
        fprintf(stderr, "%s%u", i != 0U ? "," : "", cfg.groups[i]);
    fprintf(stderr,
            " pages_per_group=%zu mask_set_id=%s ready_mono_ns=%" PRIu64 "\n",
            pages_per_group, mapping.mask_set_id, mono_ns());
    fprintf(stderr, "GATE_WAIT pid=%d name=%s signal=SIGUSR1\n",
            (int)getpid(), cfg.name);
    fflush(stderr);

    if (wait_for_gate(&gate_set) != 0) {
        fprintf(stderr, "[ERROR] SIGUSR1 gate wait failed\n");
        goto out;
    }

    fprintf(stderr,
            "RESUMED pid=%d name=%s signal=SIGUSR1 resumed_mono_ns=%" PRIu64 "\n",
            (int)getpid(), cfg.name, mono_ns());
    fflush(stderr);

    start_ns = mono_ns();
    deadline_ns = start_ns + (uint64_t)cfg.duration_sec * 1000000000ULL;

    if (cfg.streams == 1U) {
        uintptr_t cur = roots[0];
        while (!g_stop && mono_ns() < deadline_ns) {
            unsigned int k;
            for (k = 0U; k < CHUNK_ITERS; ++k) {
                cur = *(volatile uintptr_t *)cur;
            }
            operations += CHUNK_ITERS;
            if (cfg.pace_us > 0U && pace_sleep_us(cfg.pace_us) != 0) {
                perror("nanosleep");
                goto out;
            }
        }
        g_sink = cur;
    } else {
        uintptr_t cur[MAX_STREAMS];
        unsigned int sidx;
        for (sidx = 0U; sidx < cfg.streams; ++sidx)
            cur[sidx] = roots[sidx];

        while (!g_stop && mono_ns() < deadline_ns) {
            unsigned int iter;
            for (iter = 0U; iter < CHUNK_ITERS; ++iter) {
                for (sidx = 0U; sidx < cfg.streams; ++sidx)
                    cur[sidx] = *(volatile uintptr_t *)cur[sidx];
            }
            operations += (uint64_t)CHUNK_ITERS * (uint64_t)cfg.streams;
            if (cfg.pace_us > 0U && pace_sleep_us(cfg.pace_us) != 0) {
                perror("nanosleep");
                goto out;
            }
        }
        for (sidx = 0U; sidx < cfg.streams; ++sidx)
            g_sink ^= cur[sidx];
    }

    {
        const uint64_t end_ns = mono_ns();
        const double elapsed = end_ns > start_ns ?
            (double)(end_ns - start_ns) / 1000000000.0 : 0.0;
        const double mops = elapsed > 0.0 ?
            (double)operations / elapsed / 1000000.0 : 0.0;
        fprintf(stderr,
                "END pid=%d name=%s role=%s elapsed_sec=%.6f operations=%" PRIu64
                " Mops_per_sec=%.3f pace_us=%u end_mono_ns=%" PRIu64 "\n",
                (int)getpid(), cfg.name, cfg.role, elapsed, operations, mops,
                cfg.pace_us, end_ns);
        fflush(stderr);
    }

    /* Verify the full pool stayed on the same PFNs. */
    {
        size_t moved = 0U;
        size_t unreadable = 0U;
        for (i = 0U; i < page_count; ++i) {
            uint64_t now_pfn;
            if (read_pfn(pagemap_fd, (uintptr_t)(pool + i * page_size),
                         page_size, &now_pfn) != 0)
                ++unreadable;
            else if (now_pfn != page_pfn[i])
                ++moved;
        }
        printf("PFN_VERIFY_ALL,name=%s,moved=%zu,unreadable=%zu,pages=%zu\n",
               cfg.name, moved, unreadable, page_count);
        fflush(stdout);
        if (moved != 0U || unreadable != 0U)
            goto out;
    }

    rc = EXIT_SUCCESS;

out:
    if (pagemap_fd >= 0)
        close(pagemap_fd);
    if (pool != MAP_FAILED && pool_bytes != 0U)
        munmap(pool, pool_bytes);
    free(page_pfn);
    free(pool_group_counts);
    free(filled);
    free(selected);
    free(addresses);
    free(reps);
    return rc;
}

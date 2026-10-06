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

static volatile uintptr_t g_sink;

struct cfg {
    size_t pool_mib;
    unsigned int hot_pages_total;
    unsigned int duration_sec;
    int cpu;
    const char *calibration_path;
    unsigned int groups[MAX_GROUPS];
    size_t nr_groups;
};

struct selected_page {
    unsigned char *addr;
    uint64_t pfn;
    unsigned int group;
};

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--pool-mib N] [--hot-pages-total N] [--duration SEC] "
            "[--groups a,b,c,d] [--cpu N] [--calibration PATH]\n",
            prog);
}

static int parse_u(const char *s, unsigned long minv, unsigned long maxv,
                   unsigned long *out)
{
    char *end = NULL;
    unsigned long v;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < minv || v > maxv)
        return -1;
    *out = v;
    return 0;
}

static int parse_groups(const char *text, struct cfg *cfg)
{
    char *copy = NULL;
    char *save = NULL;
    char *tok;
    size_t n = 0;

    copy = strdup(text);
    if (!copy)
        return -1;

    for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        unsigned long v;
        size_t i;
        if (n >= MAX_GROUPS || parse_u(tok, 0, 1000000, &v) != 0) {
            free(copy);
            return -1;
        }
        for (i = 0; i < n; ++i) {
            if (cfg->groups[i] == (unsigned int)v) {
                free(copy);
                return -1;
            }
        }
        cfg->groups[n++] = (unsigned int)v;
    }
    free(copy);
    if (n == 0)
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
    if ((entry & PAGEMAP_PRESENT) == 0 || (entry & PAGEMAP_SWAPPED) != 0)
        return -1;
    *pfn_out = entry & PAGEMAP_PFN_MASK;
    if (*pfn_out == 0)
        return -1;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t aa = *(const uint64_t *)a;
    uint64_t bb = *(const uint64_t *)b;
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
    size_t i, n = 0;

    if (mapping->mask_count >= 20U)
        return -1;
    raw_count = (size_t)1ULL << mapping->mask_count;
    tmp = calloc(raw_count, sizeof(*tmp));
    if (!tmp)
        return -1;
    for (i = 0; i < raw_count; ++i)
        tmp[i] = dram_mapping_canonical_group_fast((uint64_t)i, deltas, delta_count);
    qsort(tmp, raw_count, sizeof(*tmp), cmp_u64);
    for (i = 0; i < raw_count; ++i) {
        if (i == 0 || tmp[i] != tmp[i - 1])
            ++n;
    }
    reps = calloc(n, sizeof(*reps));
    if (!reps) {
        free(tmp);
        return -1;
    }
    n = 0;
    for (i = 0; i < raw_count; ++i) {
        if (i == 0 || tmp[i] != tmp[i - 1])
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
    uint64_t pa = pfn * (uint64_t)page_size;
    uint64_t raw = dram_mapping_bank_class_fast(mapping, pa);
    uint64_t rep = dram_mapping_canonical_group_fast(raw, deltas, delta_count);
    size_t lo = 0, hi = rep_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (reps[mid] < rep)
            lo = mid + 1;
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
    for (i = 0; i < cfg->nr_groups; ++i)
        if (cfg->groups[i] == group)
            return (int)i;
    return -1;
}

static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    if (x == 0)
        x = 0x9e3779b97f4a7c15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}

static void shuffle_pages(struct selected_page *p, size_t n)
{
    uint64_t state = 0x123456789abcdef0ULL;
    size_t i;
    for (i = n; i > 1; --i) {
        size_t j = (size_t)(rng_next(&state) % i);
        struct selected_page t = p[i - 1];
        p[i - 1] = p[j];
        p[j] = t;
    }
}

int main(int argc, char **argv)
{
    struct cfg cfg = {
        .pool_mib = 128,
        .hot_pages_total = 3072,
        .duration_sec = 12,
        .cpu = -1,
        .calibration_path = DEFAULT_CALIBRATION_PATH,
        .groups = {0, 8, 16, 24},
        .nr_groups = 4,
    };
    struct calibration_result cal;
    struct dram_mapping mapping;
    uint64_t deltas[DRAM_MAPPING_MAX_PAGE_DELTAS];
    size_t delta_count = 0;
    uint64_t *reps = NULL;
    size_t rep_count = 0;
    unsigned long v;
    long page_size_l;
    size_t page_size, pool_bytes, page_count;
    unsigned char *pool = MAP_FAILED;
    int pagemap_fd = -1;
    unsigned int *page_group = NULL;
    uint64_t *page_pfn = NULL;
    unsigned int *filled = NULL;
    uint64_t *pool_group_counts = NULL;
    struct selected_page *selected = NULL;
    size_t selected_total, selected_n = 0;
    size_t pages_per_group;
    size_t i;
    int rc = EXIT_FAILURE;
    uint64_t start_ns, end_ns;
    uint64_t reads = 0;
    uintptr_t cur;

    for (i = 1; i < (size_t)argc; ++i) {
        if (strcmp(argv[i], "--pool-mib") == 0) {
            if (++i >= (size_t)argc || parse_u(argv[i], 4, 4096, &v) != 0) { usage(argv[0]); return 2; }
            cfg.pool_mib = (size_t)v;
        } else if (strcmp(argv[i], "--hot-pages-total") == 0) {
            if (++i >= (size_t)argc || parse_u(argv[i], 1, 1000000, &v) != 0) { usage(argv[0]); return 2; }
            cfg.hot_pages_total = (unsigned int)v;
        } else if (strcmp(argv[i], "--duration") == 0) {
            if (++i >= (size_t)argc || parse_u(argv[i], 1, 86400, &v) != 0) { usage(argv[0]); return 2; }
            cfg.duration_sec = (unsigned int)v;
        } else if (strcmp(argv[i], "--cpu") == 0) {
            if (++i >= (size_t)argc || parse_u(argv[i], 0, 4095, &v) != 0) { usage(argv[0]); return 2; }
            cfg.cpu = (int)v;
        } else if (strcmp(argv[i], "--groups") == 0) {
            if (++i >= (size_t)argc || parse_groups(argv[i], &cfg) != 0) { usage(argv[0]); return 2; }
        } else if (strcmp(argv[i], "--calibration") == 0) {
            if (++i >= (size_t)argc) { usage(argv[0]); return 2; }
            cfg.calibration_path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (geteuid() != 0) {
        fprintf(stderr, "[ERROR] run as root; PFN access via pagemap is required\n");
        return 1;
    }

    page_size_l = sysconf(_SC_PAGESIZE);
    if (page_size_l <= 0)
        goto out;
    page_size = (size_t)page_size_l;
    pool_bytes = cfg.pool_mib * (size_t)MIB;
    pool_bytes = (pool_bytes / page_size) * page_size;
    page_count = pool_bytes / page_size;
    if (cfg.hot_pages_total == 0U ||
        cfg.hot_pages_total % cfg.nr_groups != 0U) {
        fprintf(stderr, "[ERROR] --hot-pages-total must be nonzero and divisible by group count\n");
        goto out;
    }
    selected_total = (size_t)cfg.hot_pages_total;
    pages_per_group = selected_total / cfg.nr_groups;
    if (selected_total >= page_count) {
        fprintf(stderr, "[ERROR] selected pages must be smaller than pool pages\n");
        goto out;
    }

    calibration_result_reset(&cal);
    dram_mapping_reset(&mapping);
    if (calibration_load_result(cfg.calibration_path, &cal) != CALIBRATION_OK) {
        fprintf(stderr, "[ERROR] cannot load calibration %s\n", cfg.calibration_path);
        goto out;
    }
    if (dram_mapping_init_from_calibration(&mapping, &cal) != DRAM_MAPPING_OK)
        goto out;
    if (dram_mapping_page_class_deltas(&mapping, page_size, CACHELINE_BYTES,
                                       deltas, NULL, DRAM_MAPPING_MAX_PAGE_DELTAS,
                                       &delta_count) != DRAM_MAPPING_OK ||
        !dram_mapping_page_deltas_form_xor_subgroup(deltas, delta_count)) {
        fprintf(stderr, "[ERROR] page delta set is not a valid XOR subgroup\n");
        goto out;
    }
    if (build_dense_reps(&mapping, deltas, delta_count, &reps, &rep_count) != 0)
        goto out;
    for (i = 0; i < cfg.nr_groups; ++i) {
        if (cfg.groups[i] >= rep_count) {
            fprintf(stderr, "[ERROR] requested dense group %u >= group_count=%zu\n",
                    cfg.groups[i], rep_count);
            goto out;
        }
    }

    printf("# mask_set_id=%s raw_class_count=%zu group_count=%zu delta_count=%zu\n",
           mapping.mask_set_id, (size_t)1ULL << mapping.mask_count,
           rep_count, delta_count);
    for (i = 0; i < rep_count; ++i)
        printf("GROUP_MAP,%zu,0x%" PRIx64 "\n", i, reps[i]);

    pool = mmap(NULL, pool_bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED)
        goto out;
    (void)madvise(pool, pool_bytes, MADV_NOHUGEPAGE);

    /* Fault every page first so SPATIAL ground truth remains the full pool. */
    for (i = 0; i < page_count; ++i)
        pool[i * page_size] = (unsigned char)(i * 17U + 3U);

    pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap_fd < 0)
        goto out;

    page_group = calloc(page_count, sizeof(*page_group));
    page_pfn = calloc(page_count, sizeof(*page_pfn));
    filled = calloc(cfg.nr_groups, sizeof(*filled));
    pool_group_counts = calloc(rep_count, sizeof(*pool_group_counts));
    selected = calloc(selected_total, sizeof(*selected));
    if (!page_group || !page_pfn || !filled || !selected || !pool_group_counts)
        goto out;

    for (i = 0; i < page_count; ++i) {
        uint64_t pfn;
        unsigned int group;
        int slot;
        if (read_pfn(pagemap_fd, (uintptr_t)(pool + i * page_size), page_size, &pfn) != 0) {
            fprintf(stderr, "[ERROR] unreadable PFN at page=%zu\n", i);
            goto out;
        }
        if (dense_group_for_pfn(&mapping, deltas, delta_count, reps, rep_count,
                                pfn, page_size, &group) != 0)
            goto out;
        page_group[i] = group;
        page_pfn[i] = pfn;
        ++pool_group_counts[group];
        slot = selected_slot(&cfg, group);
        if (slot >= 0 && filled[slot] < pages_per_group) {
            size_t idx = (size_t)slot * pages_per_group + filled[slot];
            selected[idx].addr = pool + i * page_size;
            selected[idx].pfn = pfn;
            selected[idx].group = group;
            ++filled[slot];
            ++selected_n;
        }
    }

    if (selected_n != selected_total) {
        fprintf(stderr, "[ERROR] not enough pages for requested groups: selected=%zu expected=%zu\n",
                selected_n, selected_total);
        for (i = 0; i < cfg.nr_groups; ++i)
            fprintf(stderr, "  group=%u got=%u need=%zu\n",
                    cfg.groups[i], filled[i], pages_per_group);
        goto out;
    }

    /*
     * Deliberately keep EVERY page present and RW in one unchanged VMA.
     * Only the selected pages participate in the pointer chain.  Therefore
     * SPATIAL remains broad while actual activity is group-selective.
     */
    for (i = 0; i < rep_count; ++i)
        printf("POOL_GROUP,%zu,%" PRIu64 "\n", i, pool_group_counts[i]);

    shuffle_pages(selected, selected_total);
    for (i = 0; i < selected_total; ++i) {
        uintptr_t next = (uintptr_t)selected[(i + 1U) % selected_total].addr;
        memcpy(selected[i].addr, &next, sizeof(next));
    }

    for (i = 0; i < cfg.nr_groups; ++i)
        printf("GROUND_TRUTH_GROUP,%u,0x%" PRIx64 ",%zu\n",
               cfg.groups[i], reps[cfg.groups[i]], pages_per_group);

    if (pin_cpu(cfg.cpu) != 0) {
        perror("sched_setaffinity");
        goto out;
    }

    fprintf(stderr,
            "READY pid=%d pool_start=0x%016" PRIxPTR " pool_end=0x%016" PRIxPTR
            " pool_mib=%zu selected_pages=%zu groups=",
            (int)getpid(), (uintptr_t)pool, (uintptr_t)(pool + pool_bytes),
            cfg.pool_mib, selected_total);
    for (i = 0; i < cfg.nr_groups; ++i)
        fprintf(stderr, "%s%u", i ? "," : "", cfg.groups[i]);
    fprintf(stderr, " hot_pages_total=%zu pages_per_group=%zu ready_mono_ns=%" PRIu64 "\n",
            selected_total, pages_per_group, mono_ns());
    fflush(stderr);

    if (raise(SIGSTOP) != 0)
        goto out;

    fprintf(stderr, "RESUMED pid=%d resumed_mono_ns=%" PRIu64 "\n",
            (int)getpid(), mono_ns());
    fflush(stderr);

    start_ns = mono_ns();
    end_ns = start_ns + (uint64_t)cfg.duration_sec * 1000000000ULL;
    cur = (uintptr_t)selected[0].addr;
    while (mono_ns() < end_ns) {
        unsigned int k;
        for (k = 0; k < 4096U; ++k) {
            uintptr_t next;
            memcpy(&next, (const void *)cur, sizeof(next));
            cur = next;
        }
        reads += 4096U;
    }
    g_sink = cur;

    fprintf(stderr, "END pid=%d end_mono_ns=%" PRIu64 " reads=%" PRIu64 "\n",
            (int)getpid(), mono_ns(), reads);
    fflush(stderr);

    /*
     * Full-pool PFN stability matters because POOL_GROUP is the spatial
     * ground truth used by the positive-control analysis.
     */
    {
        size_t moved = 0, unreadable = 0;
        for (i = 0; i < page_count; ++i) {
            uint64_t now_pfn;
            if (read_pfn(pagemap_fd, (uintptr_t)(pool + i * page_size),
                         page_size, &now_pfn) != 0)
                ++unreadable;
            else if (now_pfn != page_pfn[i])
                ++moved;
        }
        printf("PFN_VERIFY_ALL,moved=%zu,unreadable=%zu,pages=%zu\n",
               moved, unreadable, page_count);
        if (moved != 0 || unreadable != 0)
            goto out;
    }

    rc = EXIT_SUCCESS;

out:
    if (pagemap_fd >= 0)
        close(pagemap_fd);
    if (pool != MAP_FAILED)
        munmap(pool, pool_bytes);
    free(page_group);
    free(page_pfn);
    free(filled);
    free(pool_group_counts);
    free(selected);
    free(reps);
    return rc;
}

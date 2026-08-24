#define _GNU_SOURCE

#include "calibration.h"
#include "dram_mapping.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_CALIBRATION_PATH "/run/protected-daemon/dram-map.json"
#define CACHELINE_BYTES 64U
#define MIB_BYTES (1024ULL * 1024ULL)
#define POOL_COUNT 6U
#define DEFAULT_WORKSET_MIB 64U
#define DEFAULT_TRIALS 5U

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_SWAPPED (1ULL << 62)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1ULL)

struct config {
    size_t workset_mib;
    unsigned int trials;
    bool self_test;
};

struct memory_pool {
    const char *name;
    unsigned char *base;
    size_t bytes;
    size_t page_size;
    size_t page_count;
    uint64_t *pfns;
};

struct hist_stats {
    size_t active_classes;
    double coverage_pct;
    double max_share_pct;
    double top16_share_pct;
    double normalized_entropy;
    double effective_classes_hhi;
    double cv;
    double avg_classes_per_page;
    size_t min_classes_per_page;
    size_t max_classes_per_page;
};

struct overlap_stats {
    size_t shared_active_classes;
    size_t union_active_classes;
    double set_jaccard;
    double weighted_jaccard;
    double cosine_similarity;
    double total_variation;
};

static const char *pool_names[POOL_COUNT] = {
    "victim", "c0", "c1", "c2", "c3", "c4"
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --self-test\n"
            "  %s [--workset-mib N] [--trials N]\n\n"
            "Experiment G: natural recovered-DRAM-class distribution.\n"
            "No class-aware address selection is performed.\n",
            prog,
            prog);
}

static int parse_size(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || out == NULL || *s == '\0') {
        return -1;
    }

    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v == 0ULL || v > SIZE_MAX) {
        return -1;
    }

    *out = (size_t)v;
    return 0;
}

static int parse_uint(const char *s, unsigned int *out)
{
    size_t tmp;

    if (parse_size(s, &tmp) != 0 || tmp > UINT32_MAX) {
        return -1;
    }
    *out = (unsigned int)tmp;
    return 0;
}

static int parse_args(int argc, char **argv, struct config *cfg)
{
    int i;

    if (cfg == NULL) {
        return -1;
    }

    cfg->workset_mib = DEFAULT_WORKSET_MIB;
    cfg->trials = DEFAULT_TRIALS;
    cfg->self_test = false;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--self-test") == 0) {
            cfg->self_test = true;
        } else if (strcmp(argv[i], "--workset-mib") == 0) {
            if (++i >= argc || parse_size(argv[i], &cfg->workset_mib) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--trials") == 0) {
            if (++i >= argc || parse_uint(argv[i], &cfg->trials) != 0) {
                return -1;
            }
        } else {
            fprintf(stderr, "[ERROR] unknown argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (cfg->self_test) {
        cfg->workset_mib = 8U;
        cfg->trials = 1U;
    }

    return 0;
}

static int load_mapping(struct calibration_result *calibration,
                        struct dram_mapping *mapping)
{
    int rc;
    size_t i;

    calibration_result_reset(calibration);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(DEFAULT_CALIBRATION_PATH, calibration);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr, "[ERROR] calibration_load_result: %s\n",
                calibration_strerror(rc));
        return -1;
    }

    rc = dram_mapping_init_from_calibration(mapping, calibration);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr, "[ERROR] dram_mapping_init_from_calibration: %s\n",
                dram_mapping_strerror(rc));
        return -1;
    }

    printf("[MAPPING] calibration=%s\n", DEFAULT_CALIBRATION_PATH);
    printf("[MAPPING] mask_set_id=%s\n", mapping->mask_set_id);
    printf("[MAPPING] mask_count=%zu\n", mapping->mask_count);
    for (i = 0U; i < mapping->mask_count; ++i) {
        printf("[MAPPING] mask[%zu]=0x%016" PRIx64 "\n",
               i, mapping->masks[i]);
    }

    return 0;
}

static int read_one_pfn(int pagemap_fd,
                        uintptr_t va,
                        size_t page_size,
                        uint64_t *pfn_out)
{
    uint64_t entry;
    uint64_t vpn;
    off_t off;
    ssize_t n;

    if (pfn_out == NULL || page_size == 0U) {
        return -1;
    }

    vpn = (uint64_t)(va / (uintptr_t)page_size);
    off = (off_t)(vpn * sizeof(entry));
    n = pread(pagemap_fd, &entry, sizeof(entry), off);
    if (n != (ssize_t)sizeof(entry)) {
        return -1;
    }
    if ((entry & PAGEMAP_PRESENT) == 0U) {
        return -2;
    }
    if ((entry & PAGEMAP_SWAPPED) != 0U) {
        return -3;
    }

    *pfn_out = entry & PAGEMAP_PFN_MASK;
    return (*pfn_out == 0U) ? -4 : 0;
}

static void pool_destroy(struct memory_pool *pool)
{
    if (pool == NULL) {
        return;
    }
    free(pool->pfns);
    pool->pfns = NULL;
    if (pool->base != NULL && pool->base != MAP_FAILED) {
        (void)munmap(pool->base, pool->bytes);
    }
    memset(pool, 0, sizeof(*pool));
}

static int pool_create(struct memory_pool *pool,
                       const char *name,
                       size_t workset_mib,
                       size_t page_size,
                       unsigned int salt)
{
    int fd = -1;
    volatile unsigned char *p;
    size_t i;
    uint64_t valid = 0U, absent = 0U, swapped = 0U, unavailable = 0U;

    memset(pool, 0, sizeof(*pool));
    pool->name = name;
    pool->bytes = workset_mib * (size_t)MIB_BYTES;
    pool->page_size = page_size;

    if (pool->bytes == 0U || pool->bytes % page_size != 0U) {
        return -1;
    }
    pool->page_count = pool->bytes / page_size;

    pool->base = mmap(NULL,
                      pool->bytes,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS,
                      -1,
                      0);
    if (pool->base == MAP_FAILED) {
        pool->base = NULL;
        perror("mmap");
        return -1;
    }

    if (madvise(pool->base, pool->bytes, MADV_NOHUGEPAGE) != 0) {
        perror("madvise(MADV_NOHUGEPAGE)");
        pool_destroy(pool);
        return -1;
    }

    pool->pfns = calloc(pool->page_count, sizeof(pool->pfns[0]));
    if (pool->pfns == NULL) {
        pool_destroy(pool);
        return -1;
    }

    p = (volatile unsigned char *)pool->base;
    for (i = 0U; i < pool->page_count; ++i) {
        p[i * page_size] = (unsigned char)((i + (size_t)salt * 131U) & 0xffU);
    }

    fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open(/proc/self/pagemap)");
        pool_destroy(pool);
        return -1;
    }

    for (i = 0U; i < pool->page_count; ++i) {
        uint64_t pfn = 0U;
        int rc = read_one_pfn(fd,
                              (uintptr_t)(pool->base + i * page_size),
                              page_size,
                              &pfn);
        if (rc == 0) {
            pool->pfns[i] = pfn;
            ++valid;
        } else if (rc == -2) {
            ++absent;
        } else if (rc == -3) {
            ++swapped;
        } else {
            ++unavailable;
        }
    }
    (void)close(fd);

    printf("[POOL:%s] mmap=%p bytes=%zu pages=%zu\n",
           name, (void *)pool->base, pool->bytes, pool->page_count);
    printf("[PAGEMAP:%s] valid=%" PRIu64
           " not_present=%" PRIu64
           " swapped=%" PRIu64
           " pfn_unavailable=%" PRIu64 "\n",
           name, valid, absent, swapped, unavailable);

    if (valid != (uint64_t)pool->page_count) {
        fprintf(stderr, "[ERROR] pool %s does not have 100%% valid PFNs\n", name);
        pool_destroy(pool);
        return -1;
    }

    return 0;
}

static int class_space_from_mapping(const struct dram_mapping *mapping,
                                    size_t *out)
{
    uint64_t space;

    if (mapping == NULL || out == NULL || !dram_mapping_is_ready(mapping)) {
        return -1;
    }
    if (mapping->mask_count > 20U) {
        return -1;
    }
    space = 1ULL << mapping->mask_count;
    if (space > (uint64_t)SIZE_MAX) {
        return -1;
    }
    *out = (size_t)space;
    return 0;
}

static int build_offset_classes(const struct dram_mapping *mapping,
                                size_t page_size,
                                uint64_t **offset_classes_out,
                                size_t *lines_per_page_out)
{
    size_t lines_per_page;
    uint64_t *offset_classes;
    size_t i, line;

    if (page_size % CACHELINE_BYTES != 0U) {
        return -1;
    }
    lines_per_page = page_size / CACHELINE_BYTES;
    offset_classes = calloc(lines_per_page, sizeof(offset_classes[0]));
    if (offset_classes == NULL) {
        return -1;
    }

    printf("[OFFSET] page_size=%zu cacheline=%u lines_per_page=%zu\n",
           page_size, CACHELINE_BYTES, lines_per_page);
    for (i = 0U; i < mapping->mask_count; ++i) {
        uint64_t bits = mapping->masks[i] & ((uint64_t)page_size - 1ULL);
        printf("[OFFSET] mask[%zu]_page_offset_bits=0x%03" PRIx64 "\n", i, bits);
    }

    for (line = 0U; line < lines_per_page; ++line) {
        uint64_t offset = (uint64_t)(line * CACHELINE_BYTES);
        offset_classes[line] = dram_mapping_bank_class_fast(mapping, offset);
    }

    *offset_classes_out = offset_classes;
    *lines_per_page_out = lines_per_page;
    return 0;
}

static int histogram_and_page_stats(const struct memory_pool *pool,
                                    const struct dram_mapping *mapping,
                                    const uint64_t *offset_classes,
                                    size_t lines_per_page,
                                    uint64_t *counts,
                                    size_t class_space,
                                    double *avg_classes_per_page,
                                    size_t *min_classes_per_page,
                                    size_t *max_classes_per_page)
{
    size_t page, line;
    uint64_t sum_unique = 0U;
    size_t min_unique = SIZE_MAX;
    size_t max_unique = 0U;
    unsigned char *seen;

    memset(counts, 0, class_space * sizeof(counts[0]));
    seen = calloc(class_space, sizeof(seen[0]));
    if (seen == NULL) {
        return -1;
    }

    for (page = 0U; page < pool->page_count; ++page) {
        uint64_t base_pa = pool->pfns[page] * (uint64_t)pool->page_size;
        uint64_t base_class = dram_mapping_bank_class_fast(mapping, base_pa);
        size_t unique = 0U;

        memset(seen, 0, class_space * sizeof(seen[0]));
        for (line = 0U; line < lines_per_page; ++line) {
            uint64_t cls = base_class ^ offset_classes[line];
            if (cls >= (uint64_t)class_space) {
                free(seen);
                return -1;
            }
            ++counts[cls];
            if (seen[cls] == 0U) {
                seen[cls] = 1U;
                ++unique;
            }
        }

        sum_unique += (uint64_t)unique;
        if (unique < min_unique) {
            min_unique = unique;
        }
        if (unique > max_unique) {
            max_unique = unique;
        }
    }

    free(seen);
    *avg_classes_per_page = (double)sum_unique / (double)pool->page_count;
    *min_classes_per_page = min_unique;
    *max_classes_per_page = max_unique;
    return 0;
}


static int cmp_u64_asc(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static int verify_unique_live_pfns(const struct memory_pool *pools,
                                   size_t pool_count,
                                   uint64_t *duplicate_out)
{
    uint64_t *all = NULL;
    size_t total = 0U, pos = 0U, p, i;
    uint64_t dup = 0U;

    if (pools == NULL || duplicate_out == NULL) return -1;
    for (p = 0U; p < pool_count; ++p) total += pools[p].page_count;
    all = malloc(total * sizeof(all[0]));
    if (all == NULL) return -1;
    for (p = 0U; p < pool_count; ++p) {
        for (i = 0U; i < pools[p].page_count; ++i) all[pos++] = pools[p].pfns[i];
    }
    qsort(all, total, sizeof(all[0]), cmp_u64_asc);
    for (i = 1U; i < total; ++i) {
        if (all[i] == all[i - 1U]) ++dup;
    }
    free(all);
    *duplicate_out = dup;
    return 0;
}

static int cmp_u64_desc(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    if (x < y) return 1;
    if (x > y) return -1;
    return 0;
}

static int compute_hist_stats(const uint64_t *counts,
                              size_t class_space,
                              uint64_t total,
                              double avg_classes_per_page,
                              size_t min_classes_per_page,
                              size_t max_classes_per_page,
                              struct hist_stats *out)
{
    size_t i;
    size_t active = 0U;
    uint64_t max_count = 0U;
    double entropy = 0.0;
    double hhi = 0.0;
    double mean;
    double var = 0.0;
    uint64_t *sorted;
    uint64_t top16 = 0U;
    size_t topn;

    if (counts == NULL || out == NULL || class_space == 0U || total == 0U) {
        return -1;
    }

    sorted = malloc(class_space * sizeof(sorted[0]));
    if (sorted == NULL) {
        return -1;
    }
    memcpy(sorted, counts, class_space * sizeof(sorted[0]));
    qsort(sorted, class_space, sizeof(sorted[0]), cmp_u64_desc);

    mean = (double)total / (double)class_space;
    for (i = 0U; i < class_space; ++i) {
        double d = (double)counts[i] - mean;
        double p = (double)counts[i] / (double)total;
        if (counts[i] > 0U) {
            ++active;
            entropy -= p * log(p);
        }
        if (counts[i] > max_count) {
            max_count = counts[i];
        }
        hhi += p * p;
        var += d * d;
    }

    topn = class_space < 16U ? class_space : 16U;
    for (i = 0U; i < topn; ++i) {
        top16 += sorted[i];
    }
    free(sorted);

    out->active_classes = active;
    out->coverage_pct = 100.0 * (double)active / (double)class_space;
    out->max_share_pct = 100.0 * (double)max_count / (double)total;
    out->top16_share_pct = 100.0 * (double)top16 / (double)total;
    out->normalized_entropy = (class_space > 1U) ? entropy / log((double)class_space) : 1.0;
    out->effective_classes_hhi = (hhi > 0.0) ? 1.0 / hhi : 0.0;
    out->cv = (mean > 0.0) ? sqrt(var / (double)class_space) / mean : 0.0;
    out->avg_classes_per_page = avg_classes_per_page;
    out->min_classes_per_page = min_classes_per_page;
    out->max_classes_per_page = max_classes_per_page;
    return 0;
}

static void compute_overlap(const uint64_t *a,
                            const uint64_t *b,
                            size_t class_space,
                            struct overlap_stats *out)
{
    size_t i;
    size_t shared = 0U, uni = 0U;
    long double min_sum = 0.0L, max_sum = 0.0L;
    long double dot = 0.0L, na = 0.0L, nb = 0.0L;
    long double suma = 0.0L, sumb = 0.0L;
    long double tv = 0.0L;

    memset(out, 0, sizeof(*out));

    for (i = 0U; i < class_space; ++i) {
        if (a[i] > 0U || b[i] > 0U) ++uni;
        if (a[i] > 0U && b[i] > 0U) ++shared;
        min_sum += (a[i] < b[i]) ? a[i] : b[i];
        max_sum += (a[i] > b[i]) ? a[i] : b[i];
        dot += (long double)a[i] * (long double)b[i];
        na += (long double)a[i] * (long double)a[i];
        nb += (long double)b[i] * (long double)b[i];
        suma += a[i];
        sumb += b[i];
    }

    if (suma > 0.0L && sumb > 0.0L) {
        for (i = 0U; i < class_space; ++i) {
            long double pa = (long double)a[i] / suma;
            long double pb = (long double)b[i] / sumb;
            tv += fabsl(pa - pb);
        }
    }

    out->shared_active_classes = shared;
    out->union_active_classes = uni;
    out->set_jaccard = (uni > 0U) ? (double)shared / (double)uni : 0.0;
    out->weighted_jaccard = (max_sum > 0.0L) ? (double)(min_sum / max_sum) : 0.0;
    out->cosine_similarity = (na > 0.0L && nb > 0.0L) ?
        (double)(dot / sqrtl(na * nb)) : 0.0;
    out->total_variation = 0.5 * (double)tv;
}

static void print_histogram(const char *name,
                            const uint64_t *counts,
                            size_t class_space)
{
    size_t i;
    printf("[HIST:%s]", name);
    for (i = 0U; i < class_space; ++i) {
        printf(" %zu=%" PRIu64, i, counts[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    struct config cfg;
    struct calibration_result calibration;
    struct dram_mapping mapping;
    size_t page_size;
    size_t class_space = 0U;
    uint64_t *offset_classes = NULL;
    size_t lines_per_page = 0U;
    unsigned int trial;
    int status = EXIT_FAILURE;

    if (parse_args(argc, argv, &cfg) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0U || page_size == (size_t)-1) {
        fprintf(stderr, "[ERROR] invalid page size\n");
        return EXIT_FAILURE;
    }

    printf("============================================================\n");
    printf("Experiment G: natural recovered-class distribution v2\n");
    printf("============================================================\n");
    printf("[CONFIG] self_test=%s\n", cfg.self_test ? "true" : "false");
    printf("[CONFIG] page_size=%zu\n", page_size);
    printf("[CONFIG] cacheline=%u\n", CACHELINE_BYTES);
    printf("[CONFIG] workset_mib_each=%zu\n", cfg.workset_mib);
    printf("[CONFIG] pool_count=%u\n", POOL_COUNT);
    printf("[CONFIG] trials=%u\n", cfg.trials);
    printf("[CONFIG] class_aware_selection=false\n");

    if (load_mapping(&calibration, &mapping) != 0) {
        goto out;
    }
    if (class_space_from_mapping(&mapping, &class_space) != 0) {
        fprintf(stderr, "[ERROR] invalid class space\n");
        goto out;
    }
    printf("[MAPPING] possible_class_space=%zu\n", class_space);

    if (build_offset_classes(&mapping,
                             page_size,
                             &offset_classes,
                             &lines_per_page) != 0) {
        fprintf(stderr, "[ERROR] build_offset_classes failed\n");
        goto out;
    }

    printf("RESULT_CSV_HEADER,trial,pool,workset_mib,total_lines,active_classes,class_space,coverage_pct,max_share_pct,top16_share_pct,normalized_entropy,effective_classes_hhi,cv,avg_classes_per_page,min_classes_per_page,max_classes_per_page\n");
    printf("OVERLAP_CSV_HEADER,trial,candidate,shared_active_classes,union_active_classes,set_jaccard,weighted_jaccard,cosine_similarity,total_variation\n");

    {
        const size_t total_pool_count = (size_t)cfg.trials * POOL_COUNT;
        struct memory_pool *all_pools = calloc(total_pool_count, sizeof(all_pools[0]));
        uint64_t **all_hist = calloc(total_pool_count, sizeof(all_hist[0]));
        size_t idx;
        uint64_t duplicate_pfns = 0U;

        if (all_pools == NULL || all_hist == NULL) {
            free(all_pools);
            free(all_hist);
            goto out;
        }

        /*
         * Keep every trial's mappings resident until all trials finish.
         * This prevents the kernel from immediately recycling PFNs released by
         * trial N into trial N+1, which would make repeated trials non-independent.
         */
        for (trial = 0U; trial < cfg.trials; ++trial) {
            size_t p;
            printf("\n[ALLOC-TRIAL] %u/%u\n", trial + 1U, cfg.trials);
            for (p = 0U; p < POOL_COUNT; ++p) {
                idx = (size_t)trial * POOL_COUNT + p;
                if (pool_create(&all_pools[idx],
                                pool_names[p],
                                cfg.workset_mib,
                                page_size,
                                trial * 17U + (unsigned int)p + 1U) != 0) {
                    goto cleanup_all;
                }
                all_hist[idx] = calloc(class_space, sizeof(all_hist[idx][0]));
                if (all_hist[idx] == NULL) goto cleanup_all;
            }
        }

        if (verify_unique_live_pfns(all_pools, total_pool_count, &duplicate_pfns) != 0) {
            fprintf(stderr, "[ERROR] PFN uniqueness verification failed\n");
            goto cleanup_all;
        }
        printf("\n[VERIFY] live_pool_count=%zu duplicate_pfns_across_live_pools=%" PRIu64 "\n",
               total_pool_count, duplicate_pfns);
        if (duplicate_pfns != 0U) {
            fprintf(stderr, "[ERROR] live pools unexpectedly share PFNs\n");
            goto cleanup_all;
        }

        for (trial = 0U; trial < cfg.trials; ++trial) {
            size_t p;
            bool trial_ok = true;
            printf("\n[TRIAL] %u/%u\n", trial + 1U, cfg.trials);

            for (p = 0U; p < POOL_COUNT; ++p) {
                struct hist_stats hs;
                double avg_page_classes = 0.0;
                size_t min_page_classes = 0U, max_page_classes = 0U;
                uint64_t total_lines;
                struct memory_pool *pool;
                uint64_t *hist;

                idx = (size_t)trial * POOL_COUNT + p;
                pool = &all_pools[idx];
                hist = all_hist[idx];
                total_lines = (uint64_t)(pool->bytes / CACHELINE_BYTES);

                if (histogram_and_page_stats(pool,
                                             &mapping,
                                             offset_classes,
                                             lines_per_page,
                                             hist,
                                             class_space,
                                             &avg_page_classes,
                                             &min_page_classes,
                                             &max_page_classes) != 0 ||
                    compute_hist_stats(hist,
                                       class_space,
                                       total_lines,
                                       avg_page_classes,
                                       min_page_classes,
                                       max_page_classes,
                                       &hs) != 0) {
                    trial_ok = false;
                    break;
                }

                printf("[DIST] trial=%u pool=%s active=%zu/%zu coverage=%.3f%% top16=%.3f%% max_share=%.3f%% entropy=%.6f effective_hhi=%.3f cv=%.6f page_classes_avg=%.3f min=%zu max=%zu\n",
                       trial + 1U, pool_names[p], hs.active_classes, class_space,
                       hs.coverage_pct, hs.top16_share_pct, hs.max_share_pct,
                       hs.normalized_entropy, hs.effective_classes_hhi, hs.cv,
                       hs.avg_classes_per_page, hs.min_classes_per_page,
                       hs.max_classes_per_page);
                printf("RESULT_CSV,%u,%s,%zu,%" PRIu64 ",%zu,%zu,%.6f,%.6f,%.6f,%.9f,%.6f,%.9f,%.6f,%zu,%zu\n",
                       trial + 1U, pool_names[p], cfg.workset_mib, total_lines,
                       hs.active_classes, class_space, hs.coverage_pct,
                       hs.max_share_pct, hs.top16_share_pct,
                       hs.normalized_entropy, hs.effective_classes_hhi, hs.cv,
                       hs.avg_classes_per_page, hs.min_classes_per_page,
                       hs.max_classes_per_page);
                if (cfg.self_test || class_space <= 256U) {
                    print_histogram(pool_names[p], hist, class_space);
                }
            }

            if (!trial_ok) {
                fprintf(stderr, "[ERROR] trial %u failed\n", trial + 1U);
                goto cleanup_all;
            }

            for (p = 1U; p < POOL_COUNT; ++p) {
                struct overlap_stats os;
                uint64_t *victim_hist = all_hist[(size_t)trial * POOL_COUNT];
                uint64_t *cand_hist = all_hist[(size_t)trial * POOL_COUNT + p];
                compute_overlap(victim_hist, cand_hist, class_space, &os);
                printf("[OVERLAP] trial=%u candidate=%s shared_active=%zu union_active=%zu set_jaccard=%.6f weighted_jaccard=%.6f cosine=%.6f total_variation=%.6f\n",
                       trial + 1U, pool_names[p], os.shared_active_classes,
                       os.union_active_classes, os.set_jaccard,
                       os.weighted_jaccard, os.cosine_similarity,
                       os.total_variation);
                printf("OVERLAP_CSV,%u,%s,%zu,%zu,%.9f,%.9f,%.9f,%.9f\n",
                       trial + 1U, pool_names[p], os.shared_active_classes,
                       os.union_active_classes, os.set_jaccard,
                       os.weighted_jaccard, os.cosine_similarity,
                       os.total_variation);
            }
        }

        status = EXIT_SUCCESS;

cleanup_all:
        for (idx = 0U; idx < total_pool_count; ++idx) {
            free(all_hist[idx]);
            pool_destroy(&all_pools[idx]);
        }
        free(all_hist);
        free(all_pools);
        if (status != EXIT_SUCCESS) goto out;
    }

    printf("\n[INTERPRETATION GUIDE]\n");
    printf(" - coverage near 100%%: ordinary workset touches nearly all recovered classes.\n");
    printf(" - normalized_entropy/effective_hhi near 1.0/class_space: distribution is close to uniform.\n");
    printf(" - top16 share near 12.5%% when class_space=128: little concentration in only 16 classes.\n");
    printf(" - set_jaccard near 1.0: set-membership overlap cannot rank candidates well.\n");
    printf(" - weighted_jaccard/cosine below 1.0: class-frequency distributions may still differ.\n");
    printf(" - total_variation near 0: class-frequency distributions are very similar.\n");

    if (cfg.self_test) {
        printf("\n[SELF-TEST PASS]\n");
        printf(" - calibration loaded\n");
        printf(" - six ordinary worksets were allocated without class-aware selection\n");
        printf(" - every 64-byte line was classified using its physical address\n");
        printf(" - class coverage/concentration and victim-candidate overlaps were reported\n");
    }

    status = EXIT_SUCCESS;

out:
    free(offset_classes);
    return status;
}

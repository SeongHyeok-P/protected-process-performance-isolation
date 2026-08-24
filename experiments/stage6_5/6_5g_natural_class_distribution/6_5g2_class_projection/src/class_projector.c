#define _GNU_SOURCE
#include "class_projector.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_SWAPPED (1ULL << 62)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1ULL)
#define PAGEMAP_BATCH_ENTRIES 4096U

struct hist_stats {
    size_t active_classes;
    double coverage_pct;
    double top16_share_pct;
    double normalized_entropy;
    double effective_classes_hhi;
};

static int cmp_u64_desc(const void *a, const void *b)
{
    uint64_t aa = *(const uint64_t *)a;
    uint64_t bb = *(const uint64_t *)b;

    if (aa < bb)
        return 1;
    if (aa > bb)
        return -1;
    return 0;
}

static void compute_hist_stats(const uint64_t *hist,
                               size_t n,
                               uint64_t total,
                               struct hist_stats *out)
{
    uint64_t *sorted;
    size_t i;
    size_t active = 0U;
    size_t topn;
    long double entropy = 0.0L;
    long double hhi = 0.0L;
    long double top = 0.0L;

    memset(out, 0, sizeof(*out));
    if (hist == NULL || n == 0U || total == 0U)
        return;

    sorted = malloc(n * sizeof(*sorted));
    if (sorted == NULL)
        return;

    memcpy(sorted, hist, n * sizeof(*sorted));
    qsort(sorted, n, sizeof(*sorted), cmp_u64_desc);

    for (i = 0U; i < n; ++i) {
        if (hist[i] != 0U) {
            long double prob = (long double)hist[i] / (long double)total;

            ++active;
            entropy -= prob * logl(prob);
            hhi += prob * prob;
        }
    }

    topn = n < 16U ? n : 16U;
    for (i = 0U; i < topn; ++i)
        top += (long double)sorted[i];

    out->active_classes = active;
    out->coverage_pct = 100.0 * (double)active / (double)n;
    out->top16_share_pct =
        100.0 * (double)(top / (long double)total);
    if (n > 1U)
        out->normalized_entropy =
            (double)(entropy / logl((long double)n));
    out->effective_classes_hhi =
        hhi > 0.0L ? (double)(1.0L / hhi) : 0.0;

    free(sorted);
}

static void print_one_hist(FILE *out,
                           const char *kind,
                           const uint64_t *hist,
                           size_t n,
                           uint64_t total)
{
    struct hist_stats s;
    size_t i;

    compute_hist_stats(hist, n, total, &s);

    fprintf(out,
            "SUMMARY,%s,%" PRIu64 ",%zu,%.6f,%.6f,%.6f,%.6f\n",
            kind,
            total,
            s.active_classes,
            s.coverage_pct,
            s.top16_share_pct,
            s.normalized_entropy,
            s.effective_classes_hhi);

    for (i = 0U; i < n; ++i) {
        double share =
            total != 0U ? 100.0 * (double)hist[i] / (double)total : 0.0;

        fprintf(out,
                "CLASS,%s,%zu,%" PRIu64 ",%.9f\n",
                kind,
                i,
                hist[i],
                share);
    }
}

static void print_one_window_hist(FILE *out,
                                  uint64_t window_index,
                                  uint64_t start_ms,
                                  uint64_t end_ms,
                                  const char *kind,
                                  const uint64_t *hist,
                                  size_t n,
                                  uint64_t total,
                                  int print_classes)
{
    struct hist_stats s;
    size_t i;

    compute_hist_stats(hist, n, total, &s);

    fprintf(out,
            "WINDOW_SUMMARY,%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%s,%" PRIu64 ",%zu,%.6f,%.6f,%.6f,%.6f\n",
            window_index,
            start_ms,
            end_ms,
            kind,
            total,
            s.active_classes,
            s.coverage_pct,
            s.top16_share_pct,
            s.normalized_entropy,
            s.effective_classes_hhi);

    if (!print_classes)
        return;

    for (i = 0U; i < n; ++i) {
        double share =
            total != 0U ? 100.0 * (double)hist[i] / (double)total : 0.0;

        fprintf(out,
                "WINDOW_CLASS,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%s,%zu,%" PRIu64 ",%.9f\n",
                window_index,
                start_ms,
                end_ms,
                kind,
                i,
                hist[i],
                share);
    }
}

static void print_window_compare(FILE *out,
                                 uint64_t window_index,
                                 uint64_t start_ms,
                                 uint64_t end_ms,
                                 const char *kind_a,
                                 const uint64_t *a,
                                 uint64_t total_a,
                                 const char *kind_b,
                                 const uint64_t *b,
                                 uint64_t total_b,
                                 size_t n)
{
    size_t i;
    long double tv = 0.0L;
    long double min_sum = 0.0L;
    long double max_sum = 0.0L;
    long double dot = 0.0L;
    long double norm_a = 0.0L;
    long double norm_b = 0.0L;

    if (total_a == 0U || total_b == 0U) {
        fprintf(out,
                "WINDOW_COMPARE,%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%s,%s,nan,nan,nan\n",
                window_index, start_ms, end_ms, kind_a, kind_b);
        return;
    }

    for (i = 0U; i < n; ++i) {
        long double pa = (long double)a[i] / (long double)total_a;
        long double pb = (long double)b[i] / (long double)total_b;
        long double lo = pa < pb ? pa : pb;
        long double hi = pa > pb ? pa : pb;

        tv += fabsl(pa - pb);
        min_sum += lo;
        max_sum += hi;
        dot += pa * pb;
        norm_a += pa * pa;
        norm_b += pb * pb;
    }

    fprintf(out,
            "WINDOW_COMPARE,%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%s,%s,%.9Lf,%.9Lf,%.9Lf\n",
            window_index,
            start_ms,
            end_ms,
            kind_a,
            kind_b,
            0.5L * tv,
            max_sum > 0.0L ? min_sum / max_sum : 0.0L,
            (norm_a > 0.0L && norm_b > 0.0L)
                ? dot / sqrtl(norm_a * norm_b)
                : 0.0L);
}

static void print_compare(FILE *out,
                          const char *kind_a,
                          const uint64_t *a,
                          uint64_t total_a,
                          const char *kind_b,
                          const uint64_t *b,
                          uint64_t total_b,
                          size_t n)
{
    size_t i;
    long double tv = 0.0L;
    long double min_sum = 0.0L;
    long double max_sum = 0.0L;
    long double dot = 0.0L;
    long double norm_a = 0.0L;
    long double norm_b = 0.0L;

    if (total_a == 0U || total_b == 0U) {
        fprintf(out,
                "COMPARE,%s,%s,nan,nan,nan\n",
                kind_a,
                kind_b);
        return;
    }

    for (i = 0U; i < n; ++i) {
        long double pa = (long double)a[i] / (long double)total_a;
        long double pb = (long double)b[i] / (long double)total_b;
        long double lo = pa < pb ? pa : pb;
        long double hi = pa > pb ? pa : pb;

        tv += fabsl(pa - pb);
        min_sum += lo;
        max_sum += hi;
        dot += pa * pb;
        norm_a += pa * pa;
        norm_b += pb * pb;
    }

    fprintf(out,
            "COMPARE,%s,%s,%.9Lf,%.9Lf,%.9Lf\n",
            kind_a,
            kind_b,
            0.5L * tv,
            max_sum > 0.0L ? min_sum / max_sum : 0.0L,
            (norm_a > 0.0L && norm_b > 0.0L)
                ? dot / sqrtl(norm_a * norm_b)
                : 0.0L);
}

int g2_class_projector_init(g2_class_projector_t *p,
                            const struct dram_mapping *mapping,
                            size_t page_size,
                            unsigned int hot_threshold)
{
    size_t i;

    if (p == NULL || mapping == NULL ||
        !dram_mapping_is_ready(mapping) ||
        mapping->mask_count == 0U ||
        mapping->mask_count > G2_MAX_CLASS_BITS ||
        page_size == 0U ||
        (page_size % G2_CACHELINE_BYTES) != 0U) {
        errno = EINVAL;
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->mapping = mapping;
    p->page_size = page_size;
    p->lines_per_page = page_size / G2_CACHELINE_BYTES;
    p->class_count = (size_t)1U << mapping->mask_count;
    p->hot_threshold = hot_threshold;

    p->weighted_units = calloc(p->class_count, sizeof(uint64_t));
    p->weighted_norm_units = calloc(p->class_count, sizeof(uint64_t));
    p->hot_units = calloc(p->class_count, sizeof(uint64_t));
    p->low_active_units = calloc(p->class_count, sizeof(uint64_t));
    p->cold_units = calloc(p->class_count, sizeof(uint64_t));
    p->spatial_units = calloc(p->class_count, sizeof(uint64_t));

    p->window_weighted_units = calloc(p->class_count, sizeof(uint64_t));
    p->window_weighted_norm_units = calloc(p->class_count, sizeof(uint64_t));
    p->window_hot_units = calloc(p->class_count, sizeof(uint64_t));
    p->window_low_active_units = calloc(p->class_count, sizeof(uint64_t));
    p->window_cold_units = calloc(p->class_count, sizeof(uint64_t));
    p->window_spatial_units = calloc(p->class_count, sizeof(uint64_t));
    p->region_class_slots = calloc(p->class_count, sizeof(uint64_t));

    if (p->weighted_units == NULL ||
        p->weighted_norm_units == NULL ||
        p->hot_units == NULL ||
        p->low_active_units == NULL ||
        p->cold_units == NULL ||
        p->spatial_units == NULL ||
        p->window_weighted_units == NULL ||
        p->window_weighted_norm_units == NULL ||
        p->window_hot_units == NULL ||
        p->window_low_active_units == NULL ||
        p->window_cold_units == NULL ||
        p->window_spatial_units == NULL ||
        p->region_class_slots == NULL) {
        g2_class_projector_destroy(p);
        errno = ENOMEM;
        return -1;
    }

    /*
     * A page-aligned physical base has zero page-offset bits.  Because no
     * carry crosses the page boundary, class(base + offset) is equivalent
     * to class(base) XOR class(offset) for offsets within the page.
     *
     * Collapse all 64-byte line offsets into unique recovered-class deltas.
     * On the current 7-mask mapping this is expected to be four deltas.
     */
    for (i = 0U; i < p->lines_per_page; ++i) {
        uint64_t offset = (uint64_t)i * G2_CACHELINE_BYTES;
        uint64_t delta = dram_mapping_bank_class_fast(mapping, offset);
        size_t j;
        int found = 0;

        for (j = 0U; j < p->nr_line_deltas; ++j) {
            if (p->line_delta_class[j] == delta) {
                ++p->line_delta_count[j];
                found = 1;
                break;
            }
        }

        if (!found) {
            if (p->nr_line_deltas >= G2_MAX_LINE_DELTAS) {
                g2_class_projector_destroy(p);
                errno = EOVERFLOW;
                return -1;
            }
            p->line_delta_class[p->nr_line_deltas] = delta;
            p->line_delta_count[p->nr_line_deltas] = 1U;
            ++p->nr_line_deltas;
        }
    }

    return 0;
}

void g2_class_projector_destroy(g2_class_projector_t *p)
{
    if (p == NULL)
        return;

    free(p->weighted_units);
    free(p->weighted_norm_units);
    free(p->hot_units);
    free(p->low_active_units);
    free(p->cold_units);
    free(p->spatial_units);
    free(p->window_weighted_units);
    free(p->window_weighted_norm_units);
    free(p->window_hot_units);
    free(p->window_low_active_units);
    free(p->window_cold_units);
    free(p->window_spatial_units);
    free(p->region_class_slots);
    memset(p, 0, sizeof(*p));
}

int g2_class_projector_add_pfn(g2_class_projector_t *p,
                               uint64_t pfn,
                               unsigned int nr_accesses)
{
    uint64_t base_pa;
    uint64_t base_class;
    size_t i;

    if (p == NULL || p->mapping == NULL ||
        p->weighted_units == NULL || p->page_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    if (pfn > UINT64_MAX / (uint64_t)p->page_size) {
        errno = EOVERFLOW;
        return -1;
    }

    base_pa = pfn * (uint64_t)p->page_size;
    base_class = dram_mapping_bank_class_fast(p->mapping, base_pa);

    for (i = 0U; i < p->nr_line_deltas; ++i) {
        uint64_t cls = base_class ^ p->line_delta_class[i];
        uint64_t slots = (uint64_t)p->line_delta_count[i];
        uint64_t weighted = slots * (uint64_t)nr_accesses;

        if (cls >= p->class_count) {
            errno = ERANGE;
            return -1;
        }

        p->spatial_units[cls] += slots;
        p->spatial_total += slots;
        p->window_spatial_units[cls] += slots;
        p->window_spatial_total += slots;

        p->weighted_units[cls] += weighted;
        p->weighted_total += weighted;
        p->window_weighted_units[cls] += weighted;
        p->window_weighted_total += weighted;

        if (nr_accesses == 0U) {
            p->cold_units[cls] += slots;
            p->cold_total += slots;
            p->window_cold_units[cls] += slots;
            p->window_cold_total += slots;
        } else if (nr_accesses >= p->hot_threshold) {
            p->hot_units[cls] += slots;
            p->hot_total += slots;
            p->window_hot_units[cls] += slots;
            p->window_hot_total += slots;
        } else {
            p->low_active_units[cls] += slots;
            p->low_active_total += slots;
            p->window_low_active_units[cls] += slots;
            p->window_low_active_total += slots;
        }
    }

    return 0;
}

static int add_pfn_to_region_slots(g2_class_projector_t *p,
                                   uint64_t pfn,
                                   uint64_t *region_slot_total)
{
    uint64_t base_pa;
    uint64_t base_class;
    size_t i;

    if (p == NULL || p->mapping == NULL || p->region_class_slots == NULL ||
        region_slot_total == NULL || p->page_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    if (pfn > UINT64_MAX / (uint64_t)p->page_size) {
        errno = EOVERFLOW;
        return -1;
    }

    base_pa = pfn * (uint64_t)p->page_size;
    base_class = dram_mapping_bank_class_fast(p->mapping, base_pa);

    for (i = 0U; i < p->nr_line_deltas; ++i) {
        uint64_t cls = base_class ^ p->line_delta_class[i];
        uint64_t slots = (uint64_t)p->line_delta_count[i];

        if (cls >= p->class_count ||
            UINT64_MAX - p->region_class_slots[cls] < slots ||
            UINT64_MAX - *region_slot_total < slots) {
            errno = EOVERFLOW;
            return -1;
        }

        p->region_class_slots[cls] += slots;
        *region_slot_total += slots;
    }

    return 0;
}

static int commit_region_normalized_weight(g2_class_projector_t *p,
                                           unsigned int nr_accesses,
                                           uint64_t region_slot_total)
{
    uint64_t budget;
    size_t i;

    if (p == NULL || p->weighted_norm_units == NULL ||
        p->window_weighted_norm_units == NULL ||
        p->region_class_slots == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (nr_accesses == 0U || region_slot_total == 0U)
        return 0;

    budget = (uint64_t)nr_accesses * G2_NORM_SCALE;

    for (i = 0U; i < p->class_count; ++i) {
        uint64_t slots = p->region_class_slots[i];
        uint64_t contribution;
        long double exact;

        if (slots == 0U)
            continue;

        exact = (long double)budget * (long double)slots /
                (long double)region_slot_total;
        contribution = (uint64_t)llroundl(exact);

        if (UINT64_MAX - p->weighted_norm_units[i] < contribution ||
            UINT64_MAX - p->weighted_norm_total < contribution ||
            UINT64_MAX - p->window_weighted_norm_units[i] < contribution ||
            UINT64_MAX - p->window_weighted_norm_total < contribution) {
            errno = EOVERFLOW;
            return -1;
        }

        p->weighted_norm_units[i] += contribution;
        p->weighted_norm_total += contribution;
        p->window_weighted_norm_units[i] += contribution;
        p->window_weighted_norm_total += contribution;
    }

    return 0;
}

int g2_class_projector_project_region(g2_class_projector_t *p,
                                      int pagemap_fd,
                                      uint64_t start,
                                      uint64_t end,
                                      unsigned int nr_accesses)
{
    uint64_t first_vpn;
    uint64_t last_vpn;
    uint64_t vpn;
    uint64_t entries[PAGEMAP_BATCH_ENTRIES];
    uint64_t region_slot_total = 0U;

    if (p == NULL || pagemap_fd < 0 || end <= start || p->page_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    first_vpn = start / (uint64_t)p->page_size;
    last_vpn = (end + (uint64_t)p->page_size - 1U) /
               (uint64_t)p->page_size;

    ++p->region_samples;
    memset(p->region_class_slots, 0,
           p->class_count * sizeof(p->region_class_slots[0]));

    for (vpn = first_vpn; vpn < last_vpn;) {
        uint64_t remaining = last_vpn - vpn;
        size_t batch = remaining > PAGEMAP_BATCH_ENTRIES
                           ? PAGEMAP_BATCH_ENTRIES
                           : (size_t)remaining;
        size_t bytes = batch * sizeof(entries[0]);
        off_t off;
        ssize_t nread;
        size_t i;

        if (vpn > (uint64_t)INT64_MAX / sizeof(entries[0])) {
            errno = EOVERFLOW;
            return -1;
        }

        off = (off_t)(vpn * sizeof(entries[0]));
        nread = pread(pagemap_fd, entries, bytes, off);
        if (nread != (ssize_t)bytes) {
            if (nread >= 0)
                errno = EIO;
            return -1;
        }

        for (i = 0U; i < batch; ++i) {
            uint64_t entry = entries[i];
            uint64_t pfn;

            ++p->pages_examined;

            if ((entry & PAGEMAP_PRESENT) == 0U) {
                ++p->pages_not_present;
                continue;
            }
            if ((entry & PAGEMAP_SWAPPED) != 0U) {
                ++p->pages_swapped;
                continue;
            }

            pfn = entry & PAGEMAP_PFN_MASK;
            if (pfn == 0U) {
                ++p->pages_zero_pfn;
                continue;
            }

            ++p->pages_present;
            if (g2_class_projector_add_pfn(p, pfn, nr_accesses) < 0)
                return -1;
            if (add_pfn_to_region_slots(p, pfn, &region_slot_total) < 0)
                return -1;
        }

        vpn += (uint64_t)batch;
    }

    if (commit_region_normalized_weight(p,
                                        nr_accesses,
                                        region_slot_total) < 0)
        return -1;

    return 0;
}

void g2_class_projector_reset_window(g2_class_projector_t *p)
{
    if (p == NULL || p->class_count == 0U)
        return;

    memset(p->window_weighted_units, 0, p->class_count * sizeof(uint64_t));
    memset(p->window_weighted_norm_units, 0,
           p->class_count * sizeof(uint64_t));
    memset(p->window_hot_units, 0, p->class_count * sizeof(uint64_t));
    memset(p->window_low_active_units, 0, p->class_count * sizeof(uint64_t));
    memset(p->window_cold_units, 0, p->class_count * sizeof(uint64_t));
    memset(p->window_spatial_units, 0, p->class_count * sizeof(uint64_t));

    p->window_weighted_total = 0U;
    p->window_weighted_norm_total = 0U;
    p->window_hot_total = 0U;
    p->window_low_active_total = 0U;
    p->window_cold_total = 0U;
    p->window_spatial_total = 0U;
}

void g2_class_projector_print_window_report(const g2_class_projector_t *p,
                                            FILE *out,
                                            uint64_t window_index,
                                            uint64_t start_ms,
                                            uint64_t end_ms,
                                            unsigned int aggregations,
                                            int complete)
{
    if (p == NULL || out == NULL)
        return;

    fprintf(out,
            "WINDOW_META,%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",aggregations=%u,complete=%d\n",
            window_index,
            start_ms,
            end_ms,
            aggregations,
            complete ? 1 : 0);

    fprintf(out,
            "WINDOW_SUMMARY_HEADER,window_index,start_ms,end_ms,kind,"
            "total_units,active_classes,coverage_pct,top16_share_pct,"
            "normalized_entropy,effective_classes_hhi\n");

    print_one_window_hist(out, window_index, start_ms, end_ms,
                          "WEIGHTED", p->window_weighted_units,
                          p->class_count, p->window_weighted_total, 1);
    print_one_window_hist(out, window_index, start_ms, end_ms,
                          "WEIGHTED_NORM", p->window_weighted_norm_units,
                          p->class_count, p->window_weighted_norm_total, 1);
    print_one_window_hist(out, window_index, start_ms, end_ms,
                          "HOT", p->window_hot_units,
                          p->class_count, p->window_hot_total, 0);
    print_one_window_hist(out, window_index, start_ms, end_ms,
                          "LOW_ACTIVE", p->window_low_active_units,
                          p->class_count, p->window_low_active_total, 0);
    print_one_window_hist(out, window_index, start_ms, end_ms,
                          "COLD", p->window_cold_units,
                          p->class_count, p->window_cold_total, 0);
    print_one_window_hist(out, window_index, start_ms, end_ms,
                          "SPATIAL", p->window_spatial_units,
                          p->class_count, p->window_spatial_total, 1);

    fprintf(out,
            "WINDOW_COMPARE_HEADER,window_index,start_ms,end_ms,kind_a,"
            "kind_b,total_variation,weighted_jaccard,cosine_similarity\n");
    print_window_compare(out, window_index, start_ms, end_ms,
                         "WEIGHTED", p->window_weighted_units,
                         p->window_weighted_total,
                         "SPATIAL", p->window_spatial_units,
                         p->window_spatial_total,
                         p->class_count);
    print_window_compare(out, window_index, start_ms, end_ms,
                         "WEIGHTED_NORM", p->window_weighted_norm_units,
                         p->window_weighted_norm_total,
                         "SPATIAL", p->window_spatial_units,
                         p->window_spatial_total,
                         p->class_count);
    print_window_compare(out, window_index, start_ms, end_ms,
                         "HOT", p->window_hot_units,
                         p->window_hot_total,
                         "COLD", p->window_cold_units,
                         p->window_cold_total,
                         p->class_count);
}

void g2_class_projector_print_report(const g2_class_projector_t *p,
                                     FILE *out)
{
    size_t i;

    if (p == NULL || out == NULL)
        return;

    fprintf(out,
            "# G2 activity-weighted recovered-class projection\n");
    fprintf(out,
            "# class_count=%zu page_size=%zu cacheline=%u hot_threshold=%u "
            "page_class_deltas=%zu\n",
            p->class_count,
            p->page_size,
            G2_CACHELINE_BYTES,
            p->hot_threshold,
            p->nr_line_deltas);

    fprintf(out, "# page_class_delta_distribution=");
    for (i = 0U; i < p->nr_line_deltas; ++i) {
        if (i != 0U)
            fputc(';', out);
        fprintf(out,
                "%" PRIu64 ":%u",
                p->line_delta_class[i],
                p->line_delta_count[i]);
    }
    fputc('\n', out);

    fprintf(out,
            "PROJECTOR_STATS,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 "\n",
            p->region_samples,
            p->pages_examined,
            p->pages_present,
            p->pages_not_present,
            p->pages_swapped,
            p->pages_zero_pfn);

    fprintf(out,
            "SUMMARY_HEADER,kind,total_units,active_classes,coverage_pct,"
            "top16_share_pct,normalized_entropy,effective_classes_hhi\n");
    print_one_hist(out,
                   "WEIGHTED",
                   p->weighted_units,
                   p->class_count,
                   p->weighted_total);
    print_one_hist(out,
                   "WEIGHTED_NORM",
                   p->weighted_norm_units,
                   p->class_count,
                   p->weighted_norm_total);
    print_one_hist(out,
                   "HOT",
                   p->hot_units,
                   p->class_count,
                   p->hot_total);
    print_one_hist(out,
                   "LOW_ACTIVE",
                   p->low_active_units,
                   p->class_count,
                   p->low_active_total);
    print_one_hist(out,
                   "COLD",
                   p->cold_units,
                   p->class_count,
                   p->cold_total);
    print_one_hist(out,
                   "SPATIAL",
                   p->spatial_units,
                   p->class_count,
                   p->spatial_total);

    fprintf(out,
            "COMPARE_HEADER,kind_a,kind_b,total_variation,"
            "weighted_jaccard,cosine_similarity\n");
    print_compare(out,
                  "WEIGHTED",
                  p->weighted_units,
                  p->weighted_total,
                  "SPATIAL",
                  p->spatial_units,
                  p->spatial_total,
                  p->class_count);
    print_compare(out,
                  "WEIGHTED_NORM",
                  p->weighted_norm_units,
                  p->weighted_norm_total,
                  "SPATIAL",
                  p->spatial_units,
                  p->spatial_total,
                  p->class_count);
    print_compare(out,
                  "HOT",
                  p->hot_units,
                  p->hot_total,
                  "COLD",
                  p->cold_units,
                  p->cold_total,
                  p->class_count);
}

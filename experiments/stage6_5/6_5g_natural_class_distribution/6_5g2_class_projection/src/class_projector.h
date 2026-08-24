#ifndef G2_CLASS_PROJECTOR_H
#define G2_CLASS_PROJECTOR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dram_mapping.h"

#define G2_CACHELINE_BYTES 64U
#define G2_MAX_CLASS_BITS 16U
#define G2_MAX_LINE_DELTAS 64U
#define G2_NORM_SCALE 1000000ULL

typedef struct {
    const struct dram_mapping *mapping;
    size_t page_size;
    size_t lines_per_page;
    size_t class_count;
    unsigned int hot_threshold;

    size_t nr_line_deltas;
    uint64_t line_delta_class[G2_MAX_LINE_DELTAS];
    unsigned int line_delta_count[G2_MAX_LINE_DELTAS];

    uint64_t *weighted_units;
    uint64_t *weighted_norm_units;
    uint64_t *hot_units;
    uint64_t *low_active_units;
    uint64_t *cold_units;
    uint64_t *spatial_units;

    uint64_t weighted_total;
    uint64_t weighted_norm_total;
    uint64_t hot_total;
    uint64_t low_active_total;
    uint64_t cold_total;
    uint64_t spatial_total;

    /* Per-window accumulators.  Reset after each logical DAMON window. */
    uint64_t *window_weighted_units;
    uint64_t *window_weighted_norm_units;
    uint64_t *window_hot_units;
    uint64_t *window_low_active_units;
    uint64_t *window_cold_units;
    uint64_t *window_spatial_units;

    uint64_t window_weighted_total;
    uint64_t window_weighted_norm_total;
    uint64_t window_hot_total;
    uint64_t window_low_active_total;
    uint64_t window_cold_total;
    uint64_t window_spatial_total;

    /* Scratch histogram for one DAMON region. */
    uint64_t *region_class_slots;

    uint64_t region_samples;
    uint64_t pages_examined;
    uint64_t pages_present;
    uint64_t pages_not_present;
    uint64_t pages_swapped;
    uint64_t pages_zero_pfn;
} g2_class_projector_t;

int g2_class_projector_init(g2_class_projector_t *p,
                            const struct dram_mapping *mapping,
                            size_t page_size,
                            unsigned int hot_threshold);
void g2_class_projector_destroy(g2_class_projector_t *p);

/*
 * Project one DAMON vaddr region to recovered-class histograms.
 * WEIGHTED keeps the original page-replicated estimate for comparison.
 * WEIGHTED_NORM gives each DAMON region a total budget proportional only to
 * nr_accesses, then splits that budget by the recovered-class composition of
 * the present pages in the region.  Region size therefore does not multiply
 * the activity budget.
 */
int g2_class_projector_project_region(g2_class_projector_t *p,
                                      int pagemap_fd,
                                      uint64_t start,
                                      uint64_t end,
                                      unsigned int nr_accesses);

/* Exposed for deterministic unit testing after a PFN is already known. */
int g2_class_projector_add_pfn(g2_class_projector_t *p,
                               uint64_t pfn,
                               unsigned int nr_accesses);

void g2_class_projector_reset_window(g2_class_projector_t *p);

void g2_class_projector_print_window_report(const g2_class_projector_t *p,
                                            FILE *out,
                                            uint64_t window_index,
                                            uint64_t start_ms,
                                            uint64_t end_ms,
                                            unsigned int aggregations,
                                            int complete);

void g2_class_projector_print_report(const g2_class_projector_t *p,
                                     FILE *out);

#endif

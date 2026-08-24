#include "class_projector.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void init_test_mapping(struct dram_mapping *m)
{
    static const uint64_t masks[] = {
        0x0000000a22185100ULL,
        0x00000000888a1100ULL,
        0x0000000111002300ULL,
        0x0000000000491100ULL,
        0x000000066630c000ULL,
        0x0000000000042300ULL,
        0x0000000000410000ULL
    };
    size_t i;

    memset(m, 0, sizeof(*m));
    m->mask_count = sizeof(masks) / sizeof(masks[0]);
    for (i = 0U; i < m->mask_count; ++i)
        m->masks[i] = masks[i];
    strcpy(m->mask_set_id, "g2-test-mapping");
    m->ready = true;
}

static uint64_t sum_hist(const uint64_t *hist, size_t n)
{
    size_t i;
    uint64_t sum = 0U;

    for (i = 0U; i < n; ++i)
        sum += hist[i];

    return sum;
}

int main(void)
{
    struct dram_mapping mapping;
    g2_class_projector_t p;

    init_test_mapping(&mapping);
    assert(g2_class_projector_init(&p, &mapping, 4096U, 10U) == 0);

    assert(p.class_count == 128U);
    assert(p.lines_per_page == 64U);
    assert(p.nr_line_deltas == 4U);

    assert(g2_class_projector_add_pfn(&p, 0x12345ULL, 20U) == 0);
    assert(p.spatial_total == 64U);
    assert(p.weighted_total == 1280U);
    assert(p.hot_total == 64U);
    assert(p.low_active_total == 0U);
    assert(p.cold_total == 0U);

    assert(sum_hist(p.spatial_units, p.class_count) == 64U);
    assert(sum_hist(p.weighted_units, p.class_count) == 1280U);
    assert(sum_hist(p.hot_units, p.class_count) == 64U);

    assert(g2_class_projector_add_pfn(&p, 0x12346ULL, 2U) == 0);
    assert(p.low_active_total == 64U);
    assert(p.weighted_total == 1408U);

    assert(g2_class_projector_add_pfn(&p, 0x12347ULL, 0U) == 0);
    assert(p.cold_total == 64U);
    assert(p.spatial_total == 192U);

    g2_class_projector_destroy(&p);

    puts("[PASS] G2 class projector unit test");
    return 0;
}


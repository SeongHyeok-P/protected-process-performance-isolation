#include "calibration.h"
#include "dram_mapping.h"
#include "class_projector.h"
#include "damon_multi_observer.h"
#include "stepc_damon_detector.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL:%d: %s\n",__LINE__,#x); return 1; } } while(0)

int main(void)
{
    struct calibration_result cr;
    struct dram_mapping dm;
    g2_class_projector_t p;
    struct damon_runtime_config cfg;
    struct damon_multi_sample s;
    pid_t kp[2] = {43210, 43211};
    pid_t tp[2] = {1111, 2222};
    const uint64_t masks[] = {
        0x0000000a22185100ULL,
        0x00000000888a1100ULL,
        0x0000000111002300ULL,
        0x0000000000491100ULL,
        0x000000066630c000ULL,
        0x0000000000042300ULL,
        0x0000000000410000ULL
    };
    const char *line =
        "kdamond.1-43211 [006] ..... 1234.567890123: damon_aggregated: "
        "target_id=0 nr_regions=128 4096-8192: 7 3\n";
    size_t i;
    long ps = sysconf(_SC_PAGESIZE);

    calibration_result_reset(&cr);
    cr.status_success = true;
    cr.acceptance_passed = true;
    cr.bank_mask_count = sizeof(masks)/sizeof(masks[0]);
    for (i = 0; i < cr.bank_mask_count; i++) cr.bank_masks[i] = masks[i];
    (void)snprintf(cr.mask_set_id,sizeof(cr.mask_set_id),"80ea3e894cb0ebd8");

    dram_mapping_reset(&dm);
    CHECK(dram_mapping_init_from_calibration(&dm,&cr) == DRAM_MAPPING_OK);
    CHECK(ps > 0);
    CHECK(g2_class_projector_init(&p,&dm,(size_t)ps,1U) == 0);
    CHECK(p.raw_class_count == 128U);
    CHECK(p.class_count == 32U);
    g2_class_projector_destroy(&p);

    damon_runtime_config_frozen_stepc(&cfg);
    CHECK(cfg.sample_us == 20000UL);
    CHECK(cfg.aggr_us == 400000UL);
    CHECK(cfg.update_us == 1000000UL);
    CHECK(cfg.min_regions == 128U);
    CHECK(cfg.max_regions == 1000U);

    memset(&s,0,sizeof(s));
    CHECK(damon_multi_parse_trace_line_for_test(line,kp,tp,2,&s) == 1);
    CHECK(s.target_index == 1U);
    CHECK(s.target_pid == 2222);
    CHECK(s.kdamond_pid == 43211);
    CHECK(s.nr_regions == 128U);
    CHECK(s.nr_accesses == 7U);
    CHECK(s.trace_ts_ns == 1234567890123ULL);

    puts("PASS: 128 raw -> 32 groups, frozen 20/400 config, independent-kdamond trace mapping");
    return 0;
}


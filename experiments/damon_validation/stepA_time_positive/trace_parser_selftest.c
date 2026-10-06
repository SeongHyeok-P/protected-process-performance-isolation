#include "damon_observer.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    const char *line =
        "        kdamond.0-78459   [015] ..... 116363.596202: "
        "damon_aggregated: target_id=0 nr_regions=7 "
        "107998060707840-107998259445760: 20 1";
    pid_t pid = 1234;
    damon_region_sample_t s;
    int rc = damon_observer_parse_trace_line_for_test(line, &pid, 1U, &s);

    if (rc != 1 ||
        s.trace_ts_ns != UINT64_C(116363596202000) ||
        s.target_id != 0U || s.pid != pid || s.nr_regions != 7U ||
        s.start != UINT64_C(107998060707840) ||
        s.end != UINT64_C(107998259445760) ||
        s.nr_accesses != 20U || s.age != 1U) {
        fprintf(stderr,
                "FAIL rc=%d ts=%" PRIu64 " target=%u pid=%d nr=%u "
                "start=%" PRIu64 " end=%" PRIu64 " acc=%u age=%u\n",
                rc, s.trace_ts_ns, s.target_id, (int)s.pid, s.nr_regions,
                s.start, s.end, s.nr_accesses, s.age);
        return 1;
    }

    printf("PASS trace_timestamp_ns=%" PRIu64 "\n", s.trace_ts_ns);
    return 0;
}

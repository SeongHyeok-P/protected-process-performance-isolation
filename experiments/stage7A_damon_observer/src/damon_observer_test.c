#include "damon_observer.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const pid_t pids[] = {77684, 77685};
    damon_region_sample_t s;
    const char *line =
        "kdamond.0-78459 [015] ..... 116363.596202: "
        "damon_aggregated: target_id=1 nr_regions=10 "
        "131412294529024-131412431007744: 19 2";

    assert(damon_observer_parse_trace_line(line, pids, 2, &s) == 1);
    assert(s.target_id == 1);
    assert(s.pid == 77685);
    assert(s.nr_regions == 10);
    assert(s.start == 131412294529024ULL);
    assert(s.end == 131412431007744ULL);
    assert(s.nr_accesses == 19);
    assert(s.age == 2);

    assert(damon_observer_parse_trace_line("not a damon line", pids, 2, &s) == 0);

    puts("[PASS] damon_observer parser test");
    return 0;
}


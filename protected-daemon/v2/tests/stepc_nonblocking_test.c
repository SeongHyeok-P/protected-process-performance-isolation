#include "stepc_damon_detector.h"
#include "stepc_runtime_adapter.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned calls, available, applies;
static int fail_io, fail_apply;
static uint64_t clock_ns=1000000000ULL;
int __wrap_clock_gettime(clockid_t id, struct timespec *ts)
{
    assert(id==CLOCK_MONOTONIC);
    ts->tv_sec=(time_t)(clock_ns/1000000000ULL);
    ts->tv_nsec=(long)(clock_ns%1000000000ULL);return 0;
}
int __wrap_damon_multi_observer_poll(struct damon_multi_observer *obs,
    int timeout,damon_multi_callback cb,void *ud)
{
    (void)obs;(void)cb;(void)ud;assert(timeout==0);calls++;
    if(fail_io){errno=EIO;return -1;}
    if(available){available--;return 1;}return 0;
}
int __wrap_throttle_controller_apply_candidates(struct throttle_controller *c,
    const struct interference_candidate *a,size_t n,struct throttle_controller_stats *s)
{
    (void)c;(void)a;(void)n;(void)s;applies++;
    return fail_apply?THROTTLE_CONTROLLER_ERR_CGROUP:THROTTLE_CONTROLLER_OK;
}
static void setup(struct stepc_damon_detector *d)
{
    memset(d,0,sizeof(*d));d->started=1;
    stepc_damon_detector_config_default(&d->cfg);
    calls=available=applies=0;fail_io=fail_apply=0;
    clock_ns=1000000000ULL;
    assert(stepc_damon_detector_warmup(d,6000)==0);
    assert(calls==0);
}
int main(void)
{
    struct stepc_damon_detector d;
    struct throttle_controller ctl={0};
    struct interference_candidate out[1];
    struct throttle_controller_stats stats;
    size_t count=99;
    setup(&d);
    assert(stepc_damon_detector_tick(&d,6999999999ULL)==0);
    assert(d.state==STEPC_WARMUP);
    assert(stepc_damon_detector_tick(&d,7000000000ULL)==0);
    assert(d.state==STEPC_COLLECT);
    assert(stepc_damon_detector_tick(&d,7100000000ULL)==0);
    assert(d.profiles.window_end_ns==8100000000ULL);
    assert(stepc_damon_detector_tick(&d,8100000000ULL)==0);
    calls=0;available=20;
    assert(stepc_damon_detector_tick(&d,8200000000ULL)==0);
    assert(calls==8&&available==12); /* finite budget, backlog postpones decision */
    assert(stepc_damon_detector_tick(&d,8200000000ULL)==0);
    d.observer.readbuf_used=3;
    assert(stepc_damon_detector_tick(&d,8200000000ULL)==0);
    d.observer.readbuf_used=0;
    assert(stepc_damon_detector_tick(&d,8200000000ULL)==1);
    calls=0;
    assert(stepc_damon_detector_tick(&d,8300000000ULL)==1&&calls==0);
    assert(stepc_damon_detector_ack(&d)==0);
    assert(stepc_damon_detector_ack(&d)==-1);
    assert(stepc_damon_detector_tick(&d,8400000000ULL)==0);
    assert(d.profiles.window_start_ns==8400000000ULL);
    assert(stepc_damon_detector_tick(&d,8399999999ULL)==-1);
    fail_io=1;
    assert(stepc_damon_detector_tick(&d,8500000000ULL)==-1);
    assert(d.state==STEPC_ERROR);

    setup(&d);
    assert(stepc_runtime_refine_and_apply(&d,NULL,0,&ctl,out,1,&count,&stats)==0);
    assert(count==0&&applies==0);
    /* Empty ready window exercises once-only handoff; no live cgroup writes. */
    d.state=STEPC_DECIDE;d.protected_count=1;
    assert(stepc_runtime_refine_and_apply(&d,NULL,0,&ctl,out,1,&count,&stats)==1);
    assert(applies==1&&d.state==STEPC_COLLECT);
    assert(stepc_runtime_refine_and_apply(&d,NULL,0,&ctl,out,1,&count,&stats)==0);
    assert(applies==1);
    d.state=STEPC_DECIDE;fail_apply=1;
    assert(stepc_runtime_refine_and_apply(&d,NULL,0,&ctl,out,1,&count,&stats)==-1);
    assert(d.state==STEPC_DECIDE); /* caller must handle error, not silently ack */
    puts("PASS: warmup/collect/decide, deadlines, bounded reads, backlog, partial line, ack, errors, adapter once-only apply");
    return 0;
}

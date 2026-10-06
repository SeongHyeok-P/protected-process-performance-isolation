#include "daemon_runtime.h"
#include "process.h"
#include "candidate_filter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static uint64_t clock_ns=1000000000ULL;
static unsigned starts,stops;
static int dead;
int __wrap_clock_gettime(clockid_t id,struct timespec *t)
{
    assert(id==CLOCK_MONOTONIC);t->tv_sec=(time_t)(clock_ns/1000000000ULL);
    t->tv_nsec=(long)(clock_ns%1000000000ULL);return 0;
}
int __wrap_proc_activity_sample_now(pid_t pid,struct proc_activity_sample *s)
{
    if(dead&&pid==102)return -1;
    memset(s,0,sizeof(*s));s->valid=true;s->pid=pid;s->state='R';
    s->timestamp_ns=clock_ns;s->clock_ticks_per_sec=100;s->page_size=4096;
    s->starttime_ticks=(uint64_t)pid;s->utime_ticks=clock_ns/10000000ULL;
    s->rss_pages=65536;s->num_threads=1;return 0;
}
int __wrap_candidate_filter_classify_pid(const struct proc_info *p,pid_t daemon,struct candidate_filter_result *r)
{
    (void)daemon;r->kind=p->is_protected?CANDIDATE_FILTER_PROTECTED:CANDIDATE_FILTER_BACKGROUND;return 0;
}
int __wrap_stepc_damon_detector_start(struct stepc_damon_detector *d,const struct dram_mapping *m,
    const pid_t *p,size_t pn,const pid_t *c,size_t cn,const struct stepc_damon_detector_config *cfg)
{
    (void)m;(void)cfg;memset(d,0,sizeof(*d));d->started=1;d->state=STEPC_WARMUP;
    d->protected_count=pn;d->candidate_count=cn;d->target_count=pn+cn;
    memcpy(d->target_pids,p,pn*sizeof(*p));memcpy(d->target_pids+pn,c,cn*sizeof(*c));starts++;return 0;
}
int __wrap_stepc_damon_detector_stop(struct stepc_damon_detector *d)
{d->started=0;d->target_count=0;stops++;return 0;}
int __wrap_stepc_damon_detector_collect(struct stepc_damon_detector *d)
{assert(d->started);return 0;}
int main(void)
{
    struct daemon_runtime rt;
    struct dram_mapping dm={0};
    struct event e={0};struct proc_info *p;
    process_table_reset();e.type=EVENT_EXEC;e.pid=101;strcpy(e.comm,"protected");
    p=process_upsert_exec(&e);assert(p);p->is_protected=1;p->group=GROUP_PROTECTED;
    e.pid=102;strcpy(e.comm,"candidate");assert(process_upsert_exec(&e));
    assert(daemon_runtime_init(&rt,&dm,999,1)==0);
    assert(daemon_runtime_tick(&rt)==0);
    clock_ns+=1000000000ULL;assert(daemon_runtime_tick(&rt)==0);
    assert(starts==1&&rt.detector.candidate_count==1);
    clock_ns+=1000000000ULL;assert(daemon_runtime_tick(&rt)==0);
    assert(starts==1); /* refreshing activity must not restart warmup */
    daemon_runtime_invalidate_pid(&rt,555);assert(!rt.invalidated);
    daemon_runtime_invalidate_pid(&rt,102);assert(rt.invalidated);
    clock_ns+=100000000ULL;assert(daemon_runtime_tick(&rt)==0);
    assert(stops==1&&starts==2);
    dead=1;clock_ns+=100000000ULL;assert(daemon_runtime_tick(&rt)==0);
    assert(stops==2&&starts==2&&!rt.detector.started);
    assert(process_get(102)==NULL);
    assert(daemon_runtime_destroy(&rt)==0);
    puts("PASS: runtime Top-K connection, warmup stability, targeted invalidation, dead-target reconciliation, dry-run cleanup");
    return 0;
}

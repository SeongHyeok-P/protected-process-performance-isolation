#define _POSIX_C_SOURCE 200809L
#include "daemon_runtime.h"
#include "candidate_filter.h"
#include "policy.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
static int sample_cmp(const void *a,const void *b)
{
    pid_t x=((const struct proc_activity_sample *)a)->pid;
    pid_t y=((const struct proc_activity_sample *)b)->pid;
    return (x>y)-(x<y);
}
static int pid_cmp(const void *a,const void *b)
{
    pid_t x=*(const pid_t *)a,y=*(const pid_t *)b;return (x>y)-(x<y);
}
static const struct proc_activity_sample *find_sample(
    const struct proc_activity_sample *s,size_t n,pid_t pid)
{
    struct proc_activity_sample key={0};key.pid=pid;
    return n?bsearch(&key,s,n,sizeof(*s),sample_cmp):NULL;
}
static int alive(const struct proc_activity_sample *s)
{return s&&s->valid&&s->state!='Z'&&s->state!='X';}

void daemon_runtime_invalidate_pid(struct daemon_runtime *rt,pid_t pid)
{
    size_t i;
    for(i=0;i<rt->detector.target_count;i++)
        if(rt->detector.target_pids[i]==pid)rt->invalidated=1;
}

/* Purge dead/reused identities before controller can release a stale PID.
 * A task newly marked protected must never be moved back to background. */
static int guard_records(struct daemon_runtime *rt)
{
    size_t i=0;
    while(i<rt->controller.record_count){
        struct throttle_record *r=&rt->controller.records[i];
        struct proc_activity_sample cur;
        const struct proc_activity_sample *old=find_sample(rt->previous,rt->previous_count,r->pid);
        struct proc_info *p=process_get(r->pid);
        int protected_now=p&&(p->is_protected||policy_is_protected(p));
        int same=old&&proc_activity_sample_now(r->pid,&cur)==0&&alive(&cur)&&
                 cur.starttime_ticks==old->starttime_ticks;
        if(!same||protected_now){
            if(same&&protected_now&&!rt->dry_run&&
               cgroup_move_pid(r->pid,GROUP_PROTECTED)!=0)return -1;
            rt->controller.records[i]=rt->controller.records[--rt->controller.record_count];
        }else i++;
    }
    return 0;
}
static int release_all(struct daemon_runtime *rt)
{
    if(guard_records(rt))return -1;
    return throttle_controller_release_all(&rt->controller,NULL)==THROTTLE_CONTROLLER_OK?0:-1;
}
static int stop_measurement(struct daemon_runtime *rt)
{
    int rc=0;
    if(rt->detector.started)rc=stepc_damon_detector_stop(&rt->detector);
    if(release_all(rt))rc=-1;
    rt->invalidated=0;
    return rc;
}

static int refresh(struct daemon_runtime *rt)
{
    size_t n=process_table_count(),count=0,i,sn=0,on=0;
    struct proc_info *snapshot=calloc(n?n:1,sizeof(*snapshot));
    struct proc_activity_sample *samples=calloc(n?n:1,sizeof(*samples));
    struct activity_prefilter_observation *obs=calloc(n?n:1,sizeof(*obs));
    struct proc_activity_config acfg;
    struct activity_prefilter_config pcfg;
    int rc=-1;
    if(!snapshot||!samples||!obs)goto done;
    if(process_snapshot(snapshot,n,&count)!=PROCESS_OK)goto done;
    if(guard_records(rt))goto done;
    rt->protected_count=0;rt->top_count=0;
    proc_activity_config_default(&acfg);
    for(i=0;i<count;i++){
        struct proc_info *p=&snapshot[i];
        struct candidate_filter_result kind;
        struct proc_activity_sample cur;
        const struct proc_activity_sample *old;
        if(proc_activity_sample_now(p->pid,&cur)!=0||!alive(&cur)){
            daemon_runtime_invalidate_pid(rt,p->pid);process_remove(p->pid);continue;
        }
        old=find_sample(rt->previous,rt->previous_count,p->pid);
        if(old&&old->starttime_ticks!=cur.starttime_ticks){
            daemon_runtime_invalidate_pid(rt,p->pid);
            /* No inherited identity may cross PID reuse. */
            p->is_protected=0;p->inherited_protected=0;p->group=GROUP_UNKNOWN;
            snprintf(p->comm,sizeof(p->comm),"%.*s",(int)sizeof(p->comm)-1,cur.comm);
            {struct proc_info *live=process_get(p->pid);if(live)*live=*p;}
            old=NULL;
        }
        samples[sn++]=cur;
        if(policy_is_ignored(p)||p->pid==rt->daemon_pid||p->ppid==rt->daemon_pid)continue;
        if(policy_is_protected(p)){
            struct proc_info *live=process_get(p->pid);
            p->is_protected=1;p->group=GROUP_PROTECTED;
            if(live){live->is_protected=1;live->group=GROUP_PROTECTED;}
        }
        if(candidate_filter_classify_pid(p,rt->daemon_pid,&kind)!=0)goto done;
        if(kind.kind==CANDIDATE_FILTER_PROTECTED){
            if(rt->protected_count==DAMON_MULTI_MAX_TARGETS){
                fprintf(stderr,"[ERROR] protected targets exceed observer capacity\n");goto done;
            }
            rt->protected_pids[rt->protected_count++]=p->pid;
        }else if(kind.kind==CANDIDATE_FILTER_BACKGROUND){
            obs[on].pid=p->pid;obs[on].sample=cur;
            if(old)(void)proc_activity_compute_delta(old,&cur,&acfg,&obs[on].delta);
            on++;
        }
    }
    qsort(samples,sn,sizeof(*samples),sample_cmp);
    qsort(rt->protected_pids,rt->protected_count,sizeof(pid_t),pid_cmp);
    activity_prefilter_config_default(&pcfg);
    pcfg.max_candidates=DAMON_MULTI_MAX_TARGETS-rt->protected_count;
    if(pcfg.max_candidates&&activity_prefilter_select_topk(obs,on,&pcfg,
         rt->top,DAMON_MULTI_MAX_TARGETS,&rt->top_count,NULL)!=0)goto done;
    free(rt->previous);rt->previous=samples;rt->previous_count=sn;samples=NULL;
    rt->have_snapshot=1;rc=0;
done:
    free(snapshot);free(samples);free(obs);return rc;
}
static int targets_valid(struct daemon_runtime *rt)
{
    size_t i;
    for(i=0;i<rt->detector.target_count;i++){
        struct proc_activity_sample s;
        if(proc_activity_sample_now(rt->detector.target_pids[i],&s)!=0||!alive(&s)||
           s.starttime_ticks!=rt->target_birth[i])return 0;
    }
    return !rt->invalidated;
}
static int configure_targets(struct daemon_runtime *rt)
{
    struct stepc_damon_detector *d=&rt->detector;
    struct stepc_damon_detector_config cfg;
    pid_t cand[DAMON_MULTI_MAX_TARGETS];
    size_t i;
    int same;
    for(i=0;i<rt->top_count;i++)cand[i]=rt->top[i].pid;
    qsort(cand,rt->top_count,sizeof(pid_t),pid_cmp);
    same=d->started&&d->protected_count==rt->protected_count&&d->candidate_count==rt->top_count;
    if(same)for(i=0;i<rt->protected_count;i++)if(d->target_pids[i]!=rt->protected_pids[i])same=0;
    if(same)for(i=0;i<rt->top_count;i++)if(d->target_pids[rt->protected_count+i]!=cand[i])same=0;
    if(same)return 0;
    if(stop_measurement(rt))return -1;
    if(!rt->protected_count||!rt->top_count)return 0;
    stepc_damon_detector_config_default(&cfg);
    if(stepc_damon_detector_start(d,rt->mapping,rt->protected_pids,
         rt->protected_count,cand,rt->top_count,&cfg)!=0){
        fprintf(stderr,"[ERROR] DAMON start: %s\n",strerror(errno));return -1;
    }
    for(i=0;i<d->target_count;i++){
        const struct proc_activity_sample *s=find_sample(rt->previous,rt->previous_count,d->target_pids[i]);
        if(!s)return -1;
        rt->target_birth[i]=s->starttime_ticks;
    }
    printf("[STEPC] WARMUP protected=%zu candidates=%zu sample=20ms aggr=400ms window=1s\n",
           d->protected_count,d->candidate_count);
    return 0;
}
int daemon_runtime_init(struct daemon_runtime *rt,const struct dram_mapping *mapping,pid_t pid,int dry)
{
    struct throttle_controller_config cfg;
    memset(rt,0,sizeof(*rt));rt->mapping=mapping;rt->daemon_pid=pid;rt->dry_run=dry;
    throttle_controller_config_default(&cfg);cfg.dry_run=dry;
    return throttle_controller_init(&rt->controller,&cfg)==THROTTLE_CONTROLLER_OK?0:-1;
}
int daemon_runtime_tick(struct daemon_runtime *rt)
{
    struct interference_candidate candidates[DAMON_MULTI_MAX_TARGETS];
    struct throttle_controller_stats stats;
    uint64_t now=now_ns();
    size_t i,n=0;
    int rc;
    if(!now)return -1;
    if(!rt->have_snapshot||now-rt->last_refresh_ns>=1000000000ULL){
        if(refresh(rt))return -1;
        rt->last_refresh_ns=now;
    }
    if(rt->detector.started&&!targets_valid(rt)){
        if(stop_measurement(rt))return -1;
        if(refresh(rt))return -1;
        rt->last_refresh_ns=now;
    }
    if(!rt->detector.started)return configure_targets(rt);
    rc=stepc_damon_detector_collect(&rt->detector);
    if(rc<0)return -1;
    if(rc==0)return 0;
    /* A complete measured profile is required from every target. */
    for(i=0;i<rt->detector.target_count;i++){
        const struct damon_profile_target *t=&rt->detector.profiles.targets[i];
        if(!t->complete_bursts||!t->projected_regions||!damon_profile_activity_units(&rt->detector.profiles,i)){
            fprintf(stderr,"[STEPC] invalid/idle window pid=%d; release and skip\n",t->pid);
            if(release_all(rt)||stepc_damon_detector_ack(&rt->detector))return -1;
            return configure_targets(rt);
        }
    }
    if(guard_records(rt)||!targets_valid(rt))return -1;
    if(stepc_damon_detector_build_candidates(&rt->detector,rt->top,rt->top_count,
         candidates,DAMON_MULTI_MAX_TARGETS,&n))return -1;
    for(i=0;i<n;i++){
        struct proc_info *p=process_get(candidates[i].pid);
        if(!p||p->is_protected||policy_is_protected(p))return -1;
        printf("[STEPC] DECIDE pid=%d activity=%.6f J=%.6f score=%.6f request=%d dry_run=%d\n",
            candidates[i].pid,candidates[i].activity_score,candidates[i].overlap_score,
            candidates[i].final_score,candidates[i].should_throttle,rt->dry_run);
    }
    if(throttle_controller_apply_candidates(&rt->controller,candidates,n,&stats)!=THROTTLE_CONTROLLER_OK)return -1;
    printf("[STEPC] actions throttle=%zu release=%zu errors=%zu\n",stats.throttle_actions,stats.release_actions,stats.errors);
    if(stepc_damon_detector_ack(&rt->detector))return -1;
    return configure_targets(rt);
}
int daemon_runtime_destroy(struct daemon_runtime *rt)
{
    int rc=stop_measurement(rt);
    throttle_controller_destroy(&rt->controller);
    free(rt->previous);rt->previous=NULL;rt->previous_count=0;return rc;
}

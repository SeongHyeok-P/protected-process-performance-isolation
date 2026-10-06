#define _POSIX_C_SOURCE 200809L
#include "stepc_damon_detector.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t mono_ns(void)
{
    struct timespec ts; if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000000000ULL+(uint64_t)ts.tv_nsec;
}
static double clamp01(double x){if(x<0)return 0;if(x>1)return 1;return x;}
static const struct activity_prefilter_candidate *find_activity(const struct activity_prefilter_candidate *a,size_t n,pid_t pid)
{ size_t i;for(i=0;i<n;i++)if(a[i].pid==pid)return &a[i];return NULL; }

void stepc_damon_detector_config_default(struct stepc_damon_detector_config *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    damon_runtime_config_frozen_stepc(&cfg->damon);
    cfg->window_ms=1000U;
    /* Experiments used 6 s before measured windows. Production can retune only after validation. */
    cfg->startup_warmup_ms=6000U;
    cfg->overlap_score_threshold=0.30;
    cfg->throttle_score_threshold=0.20;
    cfg->score_mode=STEPC_SCORE_ACTIVITY_X_OVERLAP;
}
int stepc_damon_detector_start(struct stepc_damon_detector *det,const struct dram_mapping *mapping,const pid_t *prot,size_t pn,const pid_t *cand,size_t cn,const struct stepc_damon_detector_config *cfg)
{
    struct stepc_damon_detector_config local;size_t i;
    if(!det||!mapping||!prot||pn==0||(cn&& !cand)||pn>DAMON_MULTI_MAX_TARGETS||cn>DAMON_MULTI_MAX_TARGETS-pn||!dram_mapping_is_ready(mapping)){errno=EINVAL;return -1;}
    memset(det,0,sizeof(*det));if(cfg)local=*cfg;else stepc_damon_detector_config_default(&local);if(local.window_ms==0){errno=EINVAL;return -1;}det->cfg=local;det->mapping=mapping;det->protected_count=pn;det->candidate_count=cn;det->target_count=pn+cn;
    for (i = 0; i < pn; i++)
        det->target_pids[i] = prot[i];
    for (i = 0; i < cn; i++)
        det->target_pids[pn + i] = cand[i];
    if(damon_profile_window_init(&det->profiles,mapping,det->target_pids,det->target_count)!=0)return -1;
    if(damon_multi_observer_start(&det->observer,det->target_pids,det->target_count,&det->cfg.damon)!=0){damon_profile_window_destroy(&det->profiles);return -1;}
    det->started=1;
    if(stepc_damon_detector_warmup(det,det->cfg.startup_warmup_ms)!=0){
        int saved=errno;stepc_damon_detector_stop(det);errno=saved;return -1;
    }
    return 0;
}
/* Deliberately finite I/O budget: keep servicing lifecycle events under load.
 * This is cooperative, not a hard real-time bound: region projection performs
 * pagemap reads synchronously. start/stop still perform synchronous sysfs I/O.
 */
#define STEPC_READ_BUDGET 8U
#define STEPC_DRAIN_NS 50000000ULL

static void clear_partial_bursts(struct stepc_damon_detector *det)
{
    size_t i;
    for(i=0;i<det->profiles.target_count;i++) {
        struct damon_burst_state *b=&det->profiles.bursts[i];
        b->active=0;b->region_count=0;b->seen_regions=0;
    }
}

int stepc_damon_detector_warmup(struct stepc_damon_detector *det,unsigned int ms)
{
    uint64_t now;
    if(!det||!det->started){errno=EINVAL;return -1;}
    now=mono_ns();if(!now)return -1;
    det->profiles.window_active=0;
    clear_partial_bursts(det);
    det->state=STEPC_WARMUP;
    det->last_tick_ns=now;
    det->deadline_ns=now+(uint64_t)ms*1000000ULL;
    return 0; /* scheduled, never waits */
}

int stepc_damon_detector_tick(struct stepc_damon_detector *det,uint64_t now)
{
    unsigned int i;
    int rc=0;
    if(!det||!det->started||!now){errno=EINVAL;return -1;}
    if(det->state==STEPC_ERROR){errno=EIO;return -1;}
    if(now<det->last_tick_ns){errno=EINVAL;return -1;}
    det->last_tick_ns=now;
    if(det->state==STEPC_DECIDE)return 1;
    if(det->state!=STEPC_WARMUP&&det->state!=STEPC_COLLECT){errno=EINVAL;return -1;}
    if(det->state==STEPC_COLLECT&&!det->profiles.window_active){
        clear_partial_bursts(det);
        damon_profile_window_begin(&det->profiles,now,
            now+(uint64_t)det->cfg.window_ms*1000000ULL);
        det->deadline_ns=det->profiles.window_end_ns+STEPC_DRAIN_NS;
    }
    for(i=0;i<STEPC_READ_BUDGET;i++){
        rc=damon_multi_observer_poll(&det->observer,0,
            det->state==STEPC_COLLECT?damon_profile_window_on_sample:NULL,
            &det->profiles);
        if(rc<0){det->state=STEPC_ERROR;return -1;}
        if(rc==0)break;
    }
    /* A full budget means backlog may remain. Never decide from a truncated
     * queue; return to the caller and continue draining on the next tick. */
    if(rc>0||now<det->deadline_ns)return 0;
    if(det->state==STEPC_WARMUP){
        clear_partial_bursts(det);
        det->state=STEPC_COLLECT;
        return 0;
    }
    /* A partial trace line may still contain an in-window sample. */
    if(det->observer.readbuf_used)return 0;
    damon_profile_window_flush(&det->profiles);
    det->profiles.window_active=0;
    det->state=STEPC_DECIDE;
    return 1;
}

int stepc_damon_detector_ack(struct stepc_damon_detector *det)
{
    if(!det||!det->started||det->state!=STEPC_DECIDE){errno=EINVAL;return -1;}
    det->state=STEPC_COLLECT;
    return 0;
}

int stepc_damon_detector_collect(struct stepc_damon_detector *det)
{
    uint64_t now=mono_ns();
    if(!now)return -1;
    return stepc_damon_detector_tick(det,now);
}
int stepc_damon_detector_build_candidates(const struct stepc_damon_detector *det,const struct activity_prefilter_candidate *a,size_t an,struct interference_candidate *out,size_t cap,size_t *outn)
{
    size_t i,n=0,*prot; if(outn)*outn=0;if(!det||!det->started||det->state!=STEPC_DECIDE||(!a&&an)||!out||!outn)return -1;if(cap<det->candidate_count)return -1;
    prot=calloc(det->protected_count,sizeof(*prot));if(!prot)return -1;for(i=0;i<det->protected_count;i++)prot[i]=i;
    for(i=0;i<det->candidate_count;i++){
        size_t ti=det->protected_count+i;pid_t pid=det->target_pids[ti];const struct activity_prefilter_candidate *ac=find_activity(a,an,pid);double j=0,activity,final;
        if (!ac)
            continue;
        if (damon_profile_weighted_jaccard(&det->profiles, prot,
                                            det->protected_count, ti, &j) != 0)
            continue;
        activity=clamp01(ac->screening_score);final=(det->cfg.score_mode==STEPC_SCORE_OVERLAP_ONLY)?clamp01(j):clamp01(activity*j);
        memset(&out[n],0,sizeof(out[n]));out[n].pid=pid;out[n].activity_score=activity;out[n].cpu_score=ac->delta.cpu_score;out[n].fault_score=ac->delta.fault_score;out[n].rss_score=ac->delta.rss_score;out[n].overlap_score=clamp01(j);out[n].final_score=final;out[n].common_pages=0;out[n].active=true;
        out[n].should_throttle=(j>=det->cfg.overlap_score_threshold && final>=det->cfg.throttle_score_threshold);
        snprintf(out[n].reason,sizeof(out[n].reason),"stepc32 activity=%.4f overlapJ=%.4f final=%.4f units=%llu throttle=%s",activity,j,final,(unsigned long long)damon_profile_activity_units(&det->profiles,ti),out[n].should_throttle?"true":"false");n++;
    }
    free(prot);*outn=n;return 0;
}
int stepc_damon_detector_stop(struct stepc_damon_detector *det)
{
    int rc=0;if(!det)return -1;if(det->started && damon_multi_observer_stop(&det->observer)!=0)rc=-1;damon_profile_window_destroy(&det->profiles);det->started=0;det->state=STEPC_STOPPED;return rc;
}

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "damon_profile_window.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void burst_reset(struct damon_burst_state *b)
{
    if (!b)
        return;
    b->active = 0;
    b->first_ts_ns = 0;
    b->last_ts_ns = 0;
    b->expected_regions = 0;
    b->seen_regions = 0;
    b->consistent = 1;
    b->region_count = 0;
}
static int burst_reserve(struct damon_burst_state *b,size_t need)
{
    struct damon_multi_sample *n; size_t cap;
    if (need <= b->region_cap)
        return 0;
    cap = b->region_cap ? b->region_cap : 128U;
    while (cap < need)
        cap *= 2U;
    n=realloc(b->regions,cap*sizeof(*n)); if(!n)return -1; b->regions=n;b->region_cap=cap;return 0;
}
static int open_pagemap(pid_t pid)
{
    char p[64]; int n=snprintf(p,sizeof(p),"/proc/%ld/pagemap",(long)pid); if(n<=0||(size_t)n>=sizeof(p)){errno=EINVAL;return -1;} return open(p,O_RDONLY|O_CLOEXEC);
}
int damon_profile_window_init(struct damon_profile_window *pw,const struct dram_mapping *mapping,const pid_t *pids,size_t n)
{
    size_t i; long ps;
    if(!pw||!mapping||!pids||n==0||!dram_mapping_is_ready(mapping)){errno=EINVAL;return -1;}
    memset(pw,0,sizeof(*pw)); pw->mapping=mapping;pw->target_count=n;
    pw->targets=calloc(n,sizeof(*pw->targets));pw->bursts=calloc(n,sizeof(*pw->bursts)); if(!pw->targets||!pw->bursts)goto fail;
    ps=sysconf(_SC_PAGESIZE);if(ps<=0)goto fail;
    for(i=0;i<n;i++){
        pw->targets[i].pid=pids[i];pw->targets[i].pagemap_fd=open_pagemap(pids[i]);if(pw->targets[i].pagemap_fd<0)goto fail;
        if(g2_class_projector_init(&pw->targets[i].projector,mapping,(size_t)ps,1U)!=0)goto fail;
        if(i==0)pw->group_count=pw->targets[i].projector.class_count; else if(pw->group_count!=pw->targets[i].projector.class_count){errno=EPROTO;goto fail;}
        burst_reset(&pw->bursts[i]);
    }
    return 0;
fail:
    {int e=errno?errno:EIO;damon_profile_window_destroy(pw);errno=e;return -1;}
}
void damon_profile_window_destroy(struct damon_profile_window *pw)
{
    size_t i;if(!pw)return; if(pw->targets)for(i=0;i<pw->target_count;i++){if(pw->targets[i].pagemap_fd>=0)close(pw->targets[i].pagemap_fd);g2_class_projector_destroy(&pw->targets[i].projector);} if(pw->bursts)for(i=0;i<pw->target_count;i++)free(pw->bursts[i].regions);free(pw->targets);free(pw->bursts);memset(pw,0,sizeof(*pw));
}
void damon_profile_window_begin(struct damon_profile_window *pw,uint64_t start,uint64_t end)
{
    size_t i;if(!pw||end<=start)return;pw->window_start_ns=start;pw->window_end_ns=end;pw->window_active=1;for(i=0;i<pw->target_count;i++){g2_class_projector_reset_window(&pw->targets[i].projector);pw->targets[i].complete_bursts=0;pw->targets[i].discarded_bursts=0;pw->targets[i].projected_regions=0;}
}
static void commit_burst(struct damon_profile_window *pw,size_t idx)
{
    struct damon_burst_state *b; struct damon_profile_target *t; size_t i; int in_window;
    if (!pw || idx >= pw->target_count)
        return;
    b = &pw->bursts[idx];
    t = &pw->targets[idx];
    if (!b->active)
        return;
    in_window=pw->window_active && b->first_ts_ns>=pw->window_start_ns && b->first_ts_ns<pw->window_end_ns;
    if(!b->consistent||b->seen_regions!=b->expected_regions||b->region_count!=b->expected_regions){ if(in_window)t->discarded_bursts++;burst_reset(b);return; }
    if(in_window){
        t->complete_bursts++;
        for(i=0;i<b->region_count;i++){
            const struct damon_multi_sample *s=&b->regions[i];
            if(g2_class_projector_project_region(&t->projector,t->pagemap_fd,s->start,s->end,s->nr_accesses)==0)t->projected_regions++;
        }
    }
    burst_reset(b);
}
void damon_profile_window_on_sample(const struct damon_multi_sample *s,void *ud)
{
    struct damon_profile_window *pw=ud;struct damon_burst_state *b;size_t idx;
    if (!pw || !s || s->target_index >= pw->target_count)
        return;
    idx = s->target_index;
    b = &pw->bursts[idx];
    if(b->active && s->trace_ts_ns>b->last_ts_ns && s->trace_ts_ns-b->last_ts_ns>DAMON_PROFILE_BURST_GAP_NS)commit_burst(pw,idx);
    if(!b->active){b->active=1;b->first_ts_ns=s->trace_ts_ns;b->expected_regions=s->nr_regions;b->consistent=1;}
    if(s->nr_regions!=b->expected_regions)b->consistent=0;
    if(burst_reserve(b,b->region_count+1U)!=0){b->consistent=0;return;}
    b->regions[b->region_count++]=*s;b->seen_regions++;b->last_ts_ns=s->trace_ts_ns;
    /* Complete bursts can be committed immediately; no need to await the gap. */
    if(b->consistent && b->seen_regions==b->expected_regions)commit_burst(pw,idx);
}
void damon_profile_window_flush(struct damon_profile_window *pw)
{ size_t i;if(!pw)return;for(i=0;i<pw->target_count;i++)if(pw->bursts[i].active && pw->bursts[i].seen_regions==pw->bursts[i].expected_regions)commit_burst(pw,i); }

static long double norm_value(uint64_t x,uint64_t total){return total?(long double)x/(long double)total:0.0L;}
int damon_profile_weighted_jaccard(const struct damon_profile_window *pw,const size_t *prot,size_t pn,size_t ci,double *out)
{
    size_t g,p;uint64_t *agg;uint64_t at=0,ct;long double num=0,den=0;
    if(!pw||!prot||pn==0||!out||ci>=pw->target_count||pw->group_count==0)return -1;
    agg=calloc(pw->group_count,sizeof(*agg));if(!agg)return -1;
    for(p=0;p<pn;p++){if(prot[p]>=pw->target_count){free(agg);return -1;}for(g=0;g<pw->group_count;g++){uint64_t v=pw->targets[prot[p]].projector.window_weighted_norm_units[g];if(UINT64_MAX-agg[g]<v){free(agg);return -1;}agg[g]+=v;}}
    for (g = 0; g < pw->group_count; g++)
        at += agg[g];
    ct = pw->targets[ci].projector.window_weighted_norm_total;
    if(at==0||ct==0){free(agg);return -1;}
    for(g=0;g<pw->group_count;g++){long double a=norm_value(agg[g],at),b=norm_value(pw->targets[ci].projector.window_weighted_norm_units[g],ct);num+=a<b?a:b;den+=a>b?a:b;}
    free(agg);if(den<=0)return -1;*out=(double)(num/den);return 0;
}
uint64_t damon_profile_activity_units(const struct damon_profile_window *pw,size_t idx)
{if(!pw||idx>=pw->target_count)return 0;return pw->targets[idx].projector.window_weighted_norm_total;}


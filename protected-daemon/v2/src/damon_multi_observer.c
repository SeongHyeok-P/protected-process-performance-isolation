#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "damon_multi_observer.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DAMON_ROOT "/sys/kernel/mm/damon/admin"
#define TRACE_ENABLE "/sys/kernel/tracing/events/damon/damon_aggregated/enable"
#define TRACE_PIPE "/sys/kernel/tracing/trace_pipe"
#define TRACE_CLOCK "/sys/kernel/tracing/trace_clock"
#define TRACE_BUFFER "/sys/kernel/tracing/trace"

static int write_text(const char *path, const char *text)
{
    int fd; size_t len,total=0; ssize_t n;
    fd=open(path,O_WRONLY|O_CLOEXEC); if(fd<0) return -1;
    len=strlen(text);
    while(total<len){ n=write(fd,text+total,len-total); if(n<0){ if(errno==EINTR) continue; {int e=errno;close(fd);errno=e;return -1;} } if(n==0){close(fd);errno=EIO;return -1;} total+=(size_t)n; }
    return close(fd);
}
static int write_num(const char *path, unsigned long long v){ char b[64]; int n=snprintf(b,sizeof(b),"%llu",v); if(n<=0||(size_t)n>=sizeof(b)){errno=EOVERFLOW;return -1;} return write_text(path,b); }
static int read_small(const char *path,char *buf,size_t cap){ int fd; ssize_t n; if(!buf||cap<2){errno=EINVAL;return -1;} fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0)return -1; n=read(fd,buf,cap-1); {int e=errno;close(fd);errno=e;} if(n<0)return -1; buf[n]='\0'; return 0; }
static int read_int_file(const char *path,int *out){ char b[64],*e=NULL; long v; if(read_small(path,b,sizeof(b))<0)return -1; errno=0; v=strtol(b,&e,10); if(errno||e==b){errno=EPROTO;return -1;} *out=(int)v; return 0; }

void damon_runtime_config_frozen_stepc(struct damon_runtime_config *cfg)
{
    if(!cfg) return;
    cfg->sample_us=20000UL; cfg->aggr_us=400000UL; cfg->update_us=1000000UL;
    cfg->min_regions=128U; cfg->max_regions=1000U;
}
void damon_multi_observer_reset(struct damon_multi_observer *obs)
{
    size_t i; if(!obs)return; memset(obs,0,sizeof(*obs)); obs->trace_fd=-1; obs->trace_event_previous=-1;
    for(i=0;i<DAMON_MULTI_MAX_TARGETS;i++){obs->target_pids[i]=-1;obs->kdamond_pids[i]=-1;}
}

static int read_active_trace_clock(char *out,size_t cap)
{
    char b[512],*l,*r; size_t n;
    if(read_small(TRACE_CLOCK,b,sizeof(b))<0)return -1;
    l=strchr(b,'['); r=l?strchr(l+1,']'):NULL; if(!l||!r||r<=l+1){errno=EPROTO;return -1;}
    n=(size_t)(r-l-1); if(n+1>cap){errno=EOVERFLOW;return -1;} memcpy(out,l+1,n);out[n]='\0';return 0;
}
static int select_mono(struct damon_multi_observer *obs)
{
    if(read_active_trace_clock(obs->trace_clock_previous,sizeof(obs->trace_clock_previous))<0)return -1;
    if(strcmp(obs->trace_clock_previous,"mono")!=0){ if(write_text(TRACE_CLOCK,"mono")<0)return -1; obs->trace_clock_changed=1; }
    return 0;
}
static void restore_trace_state(struct damon_multi_observer *obs)
{
    char b[8];
    if(obs->trace_event_previous_valid){ snprintf(b,sizeof(b),"%d",obs->trace_event_previous?1:0); (void)write_text(TRACE_ENABLE,b); }
    if(obs->trace_clock_changed){ (void)write_text(TRACE_CLOCK,obs->trace_clock_previous); obs->trace_clock_changed=0; }
}

static int config_one(size_t k,pid_t pid,const struct damon_runtime_config *cfg)
{
    char base[256],path[320];
    if(snprintf(base,sizeof(base),DAMON_ROOT "/kdamonds/%zu",k)>=(int)sizeof(base)){errno=EOVERFLOW;return -1;}
#define WTXT(suf,val) do{ if(snprintf(path,sizeof(path),"%s/%s",base,(suf))>=(int)sizeof(path)){errno=EOVERFLOW;return -1;} if(write_text(path,(val))<0)return -1;}while(0)
#define WNUM(suf,val) do{ if(snprintf(path,sizeof(path),"%s/%s",base,(suf))>=(int)sizeof(path)){errno=EOVERFLOW;return -1;} if(write_num(path,(unsigned long long)(val))<0)return -1;}while(0)
    WTXT("state","off"); WNUM("contexts/nr_contexts",1); WTXT("contexts/0/operations","vaddr");
    WNUM("contexts/0/targets/nr_targets",1); WNUM("contexts/0/targets/0/pid_target",(unsigned long long)pid);
    WNUM("contexts/0/monitoring_attrs/intervals/sample_us",cfg->sample_us);
    WNUM("contexts/0/monitoring_attrs/intervals/aggr_us",cfg->aggr_us);
    WNUM("contexts/0/monitoring_attrs/intervals/update_us",cfg->update_us);
    WNUM("contexts/0/monitoring_attrs/nr_regions/min",cfg->min_regions);
    WNUM("contexts/0/monitoring_attrs/nr_regions/max",cfg->max_regions);
#undef WTXT
#undef WNUM
    return 0;
}

static int wait_kpid(size_t k,pid_t *out)
{
    char path[256]; int i,v;
    if(snprintf(path,sizeof(path),DAMON_ROOT "/kdamonds/%zu/pid",k)>=(int)sizeof(path)){errno=EOVERFLOW;return -1;}
    for(i=0;i<100;i++){ if(read_int_file(path,&v)==0 && v>0){*out=(pid_t)v;return 0;} usleep(20000); }
    errno=ETIMEDOUT; return -1;
}

int damon_multi_observer_start(struct damon_multi_observer *obs,const pid_t *pids,size_t n,const struct damon_runtime_config *cfg)
{
    size_t i; char path[256],b[32]; int prev=0;
    if(!obs||!pids||!cfg||n==0||n>DAMON_MULTI_MAX_TARGETS){errno=EINVAL;return -1;}
    damon_multi_observer_reset(obs); obs->cfg=*cfg; obs->target_count=n;
    for(i=0;i<n;i++){ if(pids[i]<=0){errno=EINVAL;return -1;} obs->target_pids[i]=pids[i]; }
    if(read_int_file(TRACE_ENABLE,&prev)==0){obs->trace_event_previous=prev;obs->trace_event_previous_valid=1;}
    if(select_mono(obs)<0) goto fail;
    (void)write_text(TRACE_ENABLE,"0"); (void)write_text(TRACE_BUFFER,"\n");
    /* Prototype owns the DAMON admin namespace while running. */
    if(write_text(DAMON_ROOT "/kdamonds/nr_kdamonds","0")<0)goto fail;
    snprintf(b,sizeof(b),"%zu",n); if(write_text(DAMON_ROOT "/kdamonds/nr_kdamonds",b)<0)goto fail;
    for(i=0;i<n;i++) if(config_one(i,pids[i],cfg)<0)goto fail;
    obs->trace_fd=open(TRACE_PIPE,O_RDONLY|O_NONBLOCK|O_CLOEXEC); if(obs->trace_fd<0)goto fail;
    if(write_text(TRACE_ENABLE,"1")<0)goto fail;
    for(i=0;i<n;i++){ if(snprintf(path,sizeof(path),DAMON_ROOT "/kdamonds/%zu/state",i)>=(int)sizeof(path)){errno=EOVERFLOW;goto fail;} if(write_text(path,"on")<0)goto fail; }
    for(i=0;i<n;i++) if(wait_kpid(i,&obs->kdamond_pids[i])<0)goto fail;
    obs->started=1; return 0;
fail:
    {int e=errno; (void)damon_multi_observer_stop(obs); errno=e; return -1;}
}

static int parse_ts(const char *b,const char *e,uint64_t *out)
{
    const char *d,*p; uint64_t sec=0,frac=0; unsigned dig=0;
    if (!b || !e || !out || b >= e)
        return -1;
    d = memchr(b, '.', (size_t)(e - b));
    if (!d)
        d = e;
    for(p=b;p<d;p++){if(!isdigit((unsigned char)*p))return -1;sec=sec*10U+(uint64_t)(*p-'0');}
    if(d<e)for(p=d+1;p<e;p++){if(!isdigit((unsigned char)*p))return -1;if(dig<9){frac=frac*10U+(uint64_t)(*p-'0');dig++;}}
    while(dig<9){frac*=10U;dig++;} *out=sec*1000000000ULL+frac; return 0;
}
static int parse_emitter(const char *line,const char *ts_begin,pid_t *pid)
{
    const char *p=ts_begin,*br=NULL,*dash=NULL,*q; char tmp[32]; size_t len; long v; char *end;
    for(q=line;q<ts_begin;q++) if(*q=='['){br=q;break;}
    p=br?br:ts_begin;
    while(p>line && isspace((unsigned char)p[-1]))p--;
    for(q=p;q>line;q--) if(q[-1]=='-'){dash=q;break;}
    if (!dash)
        return -1;
    len = (size_t)(p - dash);
    if (len == 0 || len >= sizeof(tmp))
        return -1;
    memcpy(tmp, dash, len);
    tmp[len] = '\0';
    v=strtol(tmp,&end,10); if(end==tmp||*end!='\0'||v<=0)return -1; *pid=(pid_t)v; return 0;
}
static int find_target_by_kpid(const pid_t *kp,const pid_t *tp,size_t n,pid_t emitter,size_t *idx,pid_t *target)
{ size_t i; for(i=0;i<n;i++)if(kp[i]==emitter){*idx=i;*target=tp[i];return 0;} return -1; }
static int parse_line_core(const char *line,const pid_t *kp,const pid_t *tp,size_t n,struct damon_multi_sample *s)
{
    const char *m,*te,*tb,*payload; unsigned tid,nr,acc,age; unsigned long long st,en; uint64_t ts; pid_t emitter,target; size_t idx;
    m=strstr(line,": damon_aggregated:"); if(!m)return 0; te=m;tb=te;while(tb>line&&!isspace((unsigned char)tb[-1]))tb--;
    if(parse_ts(tb,te,&ts)||parse_emitter(line,tb,&emitter))return -1;
    if(find_target_by_kpid(kp,tp,n,emitter,&idx,&target))return -1;
    payload=m+2; if(sscanf(payload,"damon_aggregated: target_id=%u nr_regions=%u %llu-%llu: %u %u",&tid,&nr,&st,&en,&acc,&age)!=6)return -1;
    if (tid != 0)
        return -1;
    memset(s, 0, sizeof(*s));
    s->target_index = idx;
    s->target_pid = target;
    s->kdamond_pid = emitter;
    s->nr_regions = nr;
    s->start = st;
    s->end = en;
    s->nr_accesses = acc;
    s->age = age;
    s->trace_ts_ns = ts;
    return 1;
}
int damon_multi_parse_trace_line_for_test(const char *line,const pid_t *kp,const pid_t *tp,size_t n,struct damon_multi_sample *out)
{ if(!line||!kp||!tp||!out||n==0)return -1; return parse_line_core(line,kp,tp,n,out); }

static int consume(struct damon_multi_observer *obs,damon_multi_callback cb,void *ud)
{
    size_t st=0,i; for(i=0;i<obs->readbuf_used;i++)if(obs->readbuf[i]=='\n'){ struct damon_multi_sample s; int rc; obs->readbuf[i]='\0';obs->trace_lines++; rc=parse_line_core(obs->readbuf+st,obs->kdamond_pids,obs->target_pids,obs->target_count,&s); if(rc>0){obs->matched_lines++;if(cb)cb(&s,ud);}else if(rc<0)obs->parse_fail_lines++; st=i+1; }
    if(st){size_t rem=obs->readbuf_used-st;memmove(obs->readbuf,obs->readbuf+st,rem);obs->readbuf_used=rem;} return 0;
}
int damon_multi_observer_poll(struct damon_multi_observer *obs,int timeout_ms,damon_multi_callback cb,void *ud)
{
    struct pollfd pfd; ssize_t n; int rc; if(!obs||obs->trace_fd<0){errno=EINVAL;return -1;} pfd.fd=obs->trace_fd;pfd.events=POLLIN;pfd.revents=0; rc=poll(&pfd,1,timeout_ms); if(rc<0){if(errno==EINTR)return 0;return -1;} if(rc==0)return 0;
    if (!(pfd.revents & POLLIN))
        return 0;
    if (obs->readbuf_used == sizeof(obs->readbuf)) {
        errno = ENOBUFS;
        return -1;
    }
    n = read(obs->trace_fd,
             obs->readbuf + obs->readbuf_used,
             sizeof(obs->readbuf) - obs->readbuf_used);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }
    obs->readbuf_used += (size_t)n;
    return consume(obs, cb, ud);
}
int damon_multi_observer_stop(struct damon_multi_observer *obs)
{
    size_t i; char p[256]; int rc=0; if(!obs)return -1;
    (void)write_text(TRACE_ENABLE,"0");
    for(i=0;i<obs->target_count;i++){if(snprintf(p,sizeof(p),DAMON_ROOT "/kdamonds/%zu/state",i)<(int)sizeof(p))if(write_text(p,"off")<0)rc=-1;}
    if(obs->trace_fd>=0){close(obs->trace_fd);obs->trace_fd=-1;}
    if(obs->target_count && write_text(DAMON_ROOT "/kdamonds/nr_kdamonds","0")<0)rc=-1;
    restore_trace_state(obs); obs->started=0; return rc;
}
void damon_multi_observer_destroy(struct damon_multi_observer *obs){ if(!obs)return; if(obs->started||obs->trace_fd>=0)(void)damon_multi_observer_stop(obs); damon_multi_observer_reset(obs); }


#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <inttypes.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <bpf/libbpf.h>
#include "protected_daemon.h"
#include "protected_daemon.skel.h"
#include "process.h"
#include "policy.h"
#include "cgroup.h"
#include "calibration.h"
#include "dram_mapping.h"
#include "daemon_runtime.h"

static pid_t daemon_pid;
static volatile sig_atomic_t exiting;
static int dry_run=1;
static int event_error;
enum daemon_state {
    DAEMON_STATE_STARTING, DAEMON_STATE_CONFIG_READY, DAEMON_STATE_CALIBRATING,
    DAEMON_STATE_BANK_READY, DAEMON_STATE_BPF_READY, DAEMON_STATE_RUNNING,
    DAEMON_STATE_FAILED
};
struct daemon_context {
    enum daemon_state state;
    struct calibration_result calibration;
    struct dram_mapping dram_mapping;
    bool bank_mapping_ready;
    struct daemon_runtime runtime;
};
static struct daemon_context g_ctx;
static void on_signal(int signo){(void)signo;exiting=1;}
static int libbpf_print_fn(enum libbpf_print_level level,const char *fmt,va_list args)
{return level==LIBBPF_DEBUG?0:vfprintf(stderr,fmt,args);}
static int init_dram_bank_mapping(struct daemon_context *ctx)
{
        struct calibration_config cfg = {
                .worker_path = "scripts/kk_calibrate.py",
                .kk_main_path = "../Knock-Knock/main",
                .result_path = "/run/protected-daemon/dram-map.json",
                .work_dir = "/run/protected-daemon/kk-calibration",
                .log_path = "/run/protected-daemon/calibration.log",

                .runs = 3U,
                .memory_percent = 25.0,
                .measurements = 500U,
                .timing_rounds = 50U,
                .timeout_sec = 1800U,

                .keep_csv = true
        };

        int rc;
        size_t i;

        if (!ctx)
                return -1;

        ctx->state = DAEMON_STATE_CALIBRATING;
        ctx->bank_mapping_ready = false;

        printf("[INFO] starting DRAM bank calibration...\n");

        rc = calibration_run(&cfg,&ctx->calibration);
        if (rc != CALIBRATION_OK) {
                fprintf(stderr, "[ERROR] calibration failed: %s\n",calibration_strerror(rc));

                if (ctx->calibration.error_message[0] != '\0') {
                        fprintf(stderr, "[ERROR] calibration detail: %s\n",ctx->calibration.error_message);
                }

                ctx->state = DAEMON_STATE_FAILED;
                return -1;
        }

        printf("[INFO] calibration success: masks=%zu precision=%.4f recall=%.4f\n",ctx->calibration.bank_mask_count,ctx->calibration.holdout.precision,ctx->calibration.holdout.recall);

        rc = dram_mapping_init_from_calibration(&ctx->dram_mapping,&ctx->calibration);

        if (rc != DRAM_MAPPING_OK) {
                fprintf(stderr, "[ERROR] dram mapping init failed: %s\n",dram_mapping_strerror(rc));

                ctx->state = DAEMON_STATE_FAILED;
                return -1;
        }

        ctx->bank_mapping_ready = true;
        ctx->state = DAEMON_STATE_BANK_READY;

        printf("[INFO] DRAM bank mapping ready: mask_set_id=%s mask_count=%zu\n",ctx->dram_mapping.mask_set_id,ctx->dram_mapping.mask_count);

        for (i = 0; i < ctx->dram_mapping.mask_count; i++) {
                printf("[INFO] bank_mask[%zu]=0x%016" PRIx64 "\n",i,(uint64_t)ctx->dram_mapping.masks[i]);
        }
        return 0;
}



static int handle_event(void *ctx,void *data,size_t size)
{
    struct daemon_context *dctx=ctx;
    struct event e;
    struct proc_info *p;
    if(size<sizeof(e)){fprintf(stderr,"[ERROR] short BPF event\n");event_error=1;return 0;}
    memcpy(&e,data,sizeof(e));e.comm[TASK_COMM_LEN-1]='\0';
    if(e.pid==daemon_pid||e.ppid==daemon_pid)return 0;
    if(e.type==EVENT_FORK){
        /* BPF uses TGID: a thread fork must not reset its own process record. */
        if(e.child_pid<=0||e.child_pid==e.pid)return 0;
        daemon_runtime_invalidate_pid(&dctx->runtime,e.child_pid);
        p=process_upsert_fork(&e);
    }else if(e.type==EVENT_EXEC){
        daemon_runtime_invalidate_pid(&dctx->runtime,e.pid);
        p=process_upsert_exec(&e);
    }else if(e.type==EVENT_EXIT){
        struct proc_activity_sample sample;
        /* A sched_process_exit event is per thread. Remove only when the
         * process is gone/zombie; the periodic snapshot also reconciles it. */
        if(proc_activity_sample_now(e.pid,&sample)!=0||sample.state=='Z'||sample.state=='X'){
            daemon_runtime_invalidate_pid(&dctx->runtime,e.pid);process_remove(e.pid);
        }
        return 0;
    }else return 0;
    if(!p){event_error=1;return 0;}
    if(policy_is_protected(p))p->is_protected=1;
    p->group=policy_decide_group(p);
    if(p->group==GROUP_PROTECTED&&!dry_run&&cgroup_move_pid(p->pid,GROUP_PROTECTED)!=0){
        fprintf(stderr,"[ERROR] move protected pid=%d: %s\n",p->pid,strerror(errno));event_error=1;
    }
    return 0;
}

/* Seed processes already running when BPF is attached. /proc enumeration
 * contains process leaders, so it does not turn threads into candidates. */
static int seed_processes(void)
{
    DIR *dir=opendir("/proc");struct dirent *de;
    if(!dir)return -1;
    while((de=readdir(dir))!=NULL){
        char *end,path[80],line[512];long pid;FILE *f;
        struct event e={0};
        errno=0;pid=strtol(de->d_name,&end,10);
        if(errno||*end||pid<=2||pid>INT_MAX)continue;
        e.type=EVENT_EXEC;e.pid=(int)pid;
        snprintf(path,sizeof(path),"/proc/%ld/status",pid);
        f=fopen(path,"r");if(!f)continue;
        while(fgets(line,sizeof(line),f)){
            if(!strncmp(line,"PPid:",5))e.ppid=(int)strtol(line+5,NULL,10);
            if(!strncmp(line,"Uid:",4))e.uid=(unsigned int)strtoul(line+4,NULL,10);
        }
        fclose(f);
        snprintf(path,sizeof(path),"/proc/%ld/comm",pid);
        f=fopen(path,"r");if(!f)continue;
        if(!fgets(e.comm,sizeof(e.comm),f)){fclose(f);continue;}
        fclose(f);e.comm[strcspn(e.comm,"\n")]='\0';
        handle_event(&g_ctx,&e,sizeof(e));
    }
    closedir(dir);return event_error?-1:0;
}
int main(int argc,char **argv)
{
    struct ring_buffer *rb=NULL;
    struct protected_daemon_bpf *skel=NULL;
    struct sigaction sa={0};
    const char *config_path="config/protected.conf";
    int err=1,rt_ready=0,i,config_seen=0;
    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"--apply"))dry_run=0;
        else if(!strcmp(argv[i],"--dry-run"))dry_run=1;
        else if(!strcmp(argv[i],"--help")){
            printf("Usage: %s [config/protected.conf] [--dry-run|--apply]\nRun from protected-daemon repository root.\n",argv[0]);return 0;
        }else if(argv[i][0]=='-'||config_seen){fprintf(stderr,"[ERROR] unknown argument: %s\n",argv[i]);return 1;}
        else{config_path=argv[i];config_seen=1;}
    }
    sa.sa_handler=on_signal;sigemptyset(&sa.sa_mask);
    if(sigaction(SIGINT,&sa,NULL)||sigaction(SIGTERM,&sa,NULL))return 1;
    daemon_pid=getpid();process_table_reset();
    printf("[INFO] daemon_pid=%d dry_run=%d\n",daemon_pid,dry_run);
    if(policy_load_config(config_path)<0)return 1;
    g_ctx.state=DAEMON_STATE_CONFIG_READY;
    if(init_dram_bank_mapping(&g_ctx)<0||exiting)return 1;
    libbpf_set_print(libbpf_print_fn);
    skel=protected_daemon_bpf__open_and_load();
    if(!skel){fprintf(stderr,"[ERROR] BPF open/load\n");goto cleanup;}
    if(protected_daemon_bpf__attach(skel)){fprintf(stderr,"[ERROR] BPF attach\n");goto cleanup;}
    g_ctx.state=DAEMON_STATE_BPF_READY;
    if(daemon_runtime_init(&g_ctx.runtime,&g_ctx.dram_mapping,daemon_pid,dry_run))goto cleanup;
    rt_ready=1;
    rb=ring_buffer__new(bpf_map__fd(skel->maps.rb),handle_event,&g_ctx,NULL);
    if(!rb){fprintf(stderr,"[ERROR] ring buffer creation\n");goto cleanup;}
    if(seed_processes())goto cleanup;
    g_ctx.state=DAEMON_STATE_RUNNING;
    while(!exiting){
        int rc=ring_buffer__poll(rb,10);
        if(rc==-EINTR)continue;
        if(rc<0||event_error){fprintf(stderr,"[ERROR] lifecycle processing rc=%d\n",rc);goto cleanup;}
        if(daemon_runtime_tick(&g_ctx.runtime)){
            fprintf(stderr,"[ERROR] Step C runtime; stopping and releasing controlled tasks\n");goto cleanup;
        }
    }
    err=0;
cleanup:
    if(rt_ready&&daemon_runtime_destroy(&g_ctx.runtime)){
        fprintf(stderr,"[ERROR] cleanup/release incomplete\n");err=1;
    }
    ring_buffer__free(rb);protected_daemon_bpf__destroy(skel);
    return err;
}

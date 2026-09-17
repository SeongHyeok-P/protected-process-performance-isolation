#ifndef PROTECTED_DAEMON_PROC_ACTIVITY_H
#define PROTECTED_DAEMON_PROC_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROC_ACTIVITY_COMM_MAX 256U
#define PROC_ACTIVITY_REASON_MAX 256U

enum proc_activity_rc {
    PROC_ACTIVITY_OK = 0,
    PROC_ACTIVITY_ERR_INVALID_ARG = -1,
    PROC_ACTIVITY_ERR_OPEN = -2,
    PROC_ACTIVITY_ERR_READ = -3,
    PROC_ACTIVITY_ERR_PARSE = -4,
    PROC_ACTIVITY_ERR_SYSCONF = -5,
    PROC_ACTIVITY_ERR_PID_REUSED = -6,
    PROC_ACTIVITY_ERR_NON_MONOTONIC_TIME = -7
};

struct proc_activity_config {
    double active_cpu_ratio_threshold;
    double active_faults_per_sec_threshold;
    uint64_t min_rss_pages;

    double major_fault_weight;
    double cpu_ratio_full_scale;
    double faults_per_sec_full_scale;
    double rss_mib_full_scale;
};

struct proc_activity_sample {
    bool valid;
    pid_t pid;
    char comm[PROC_ACTIVITY_COMM_MAX];
    char state;

    uint64_t timestamp_ns;
    long clock_ticks_per_sec;
    uint64_t page_size;

    uint64_t minflt;
    uint64_t majflt;
    uint64_t utime_ticks;
    uint64_t stime_ticks;
    uint64_t starttime_ticks;
    uint64_t vsize_bytes;
    int64_t rss_pages;
    int64_t num_threads;
};

struct proc_activity_delta {
    bool valid;
    bool active;
    bool pid_reused;
    pid_t pid;

    double elapsed_sec;
    uint64_t cpu_ticks_delta;
    double cpu_seconds;
    double cpu_ratio;

    uint64_t minflt_delta;
    uint64_t majflt_delta;
    double weighted_faults;
    double faults_per_sec;
    double rss_mib;

    double cpu_score;
    double fault_score;
    double rss_score;
    double total_score;

    char reason[PROC_ACTIVITY_REASON_MAX];
};

void proc_activity_config_default(struct proc_activity_config *cfg);
void proc_activity_sample_reset(struct proc_activity_sample *sample);
void proc_activity_delta_reset(struct proc_activity_delta *delta);
int proc_activity_sample_now(pid_t pid, struct proc_activity_sample *out);
int proc_activity_compute_delta(const struct proc_activity_sample *old_sample,
                                const struct proc_activity_sample *new_sample,
                                const struct proc_activity_config *cfg,
                                struct proc_activity_delta *out);
const char *proc_activity_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif


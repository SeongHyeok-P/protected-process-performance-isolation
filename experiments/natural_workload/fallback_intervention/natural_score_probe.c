#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "activity_prefilter.h"
#include "calibration.h"
#include "dram_mapping.h"
#include "interference_candidate.h"
#include "proc_activity.h"
#include "stepc_damon_detector.h"

#define NCAND 3

struct candidate_state {
    const char *label;
    pid_t pid;
    struct proc_activity_sample before;
    struct proc_activity_sample after;
    struct proc_activity_delta delta;
    struct activity_prefilter_candidate activity;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: sudo %s [--calibration PATH] "
            "VICTIM_PID BFS_PID CC_PID PR_PID\n",
            prog);
}

static int parse_pid(const char *s, pid_t *out)
{
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v <= 0)
        return -1;
    *out = (pid_t)v;
    return 0;
}

static double max3(double a, double b, double c)
{
    double m = a > b ? a : b;
    return m > c ? m : c;
}

static int load_mapping(const char *path,
                        struct calibration_result *cal,
                        struct dram_mapping *mapping)
{
    int rc;
    calibration_result_reset(cal);
    dram_mapping_reset(mapping);

    rc = calibration_load_result(path, cal);
    if (rc != CALIBRATION_OK) {
        fprintf(stderr, "[score] calibration_load_result: %s\n",
                calibration_strerror(rc));
        return -1;
    }

    rc = dram_mapping_init_from_calibration(mapping, cal);
    if (rc != DRAM_MAPPING_OK) {
        fprintf(stderr, "[score] dram_mapping_init_from_calibration: %s\n",
                dram_mapping_strerror(rc));
        return -1;
    }
    return 0;
}

static const struct interference_candidate *
find_output(const struct interference_candidate *out, size_t count, pid_t pid)
{
    size_t i;
    for (i = 0; i < count; ++i)
        if (out[i].pid == pid)
            return &out[i];
    return NULL;
}

int main(int argc, char **argv)
{
    const char *calibration_path = "/run/protected-daemon/dram-map.json";
    int argi = 1;
    pid_t victim_pid;
    pid_t candidate_pids[NCAND];
    struct candidate_state st[NCAND] = {
        {.label = "bfs"},
        {.label = "cc"},
        {.label = "pagerank"},
    };
    struct calibration_result cal;
    struct dram_mapping mapping;
    struct proc_activity_config pcfg;
    struct stepc_damon_detector_config dcfg;
    struct stepc_damon_detector detector;
    struct interference_candidate out[NCAND];
    size_t out_count = 0;
    bool detector_started = false;
    int ret = 1;
    size_t i;

    if (argc > 2 && strcmp(argv[argi], "--calibration") == 0) {
        calibration_path = argv[argi + 1];
        argi += 2;
    }
    if (argc - argi != 4) {
        usage(argv[0]);
        return 2;
    }

    if (parse_pid(argv[argi], &victim_pid) != 0 ||
        parse_pid(argv[argi + 1], &candidate_pids[0]) != 0 ||
        parse_pid(argv[argi + 2], &candidate_pids[1]) != 0 ||
        parse_pid(argv[argi + 3], &candidate_pids[2]) != 0) {
        fprintf(stderr, "[score] invalid PID\n");
        return 2;
    }
    for (i = 0; i < NCAND; ++i)
        st[i].pid = candidate_pids[i];

    if (load_mapping(calibration_path, &cal, &mapping) != 0)
        return 1;

    proc_activity_config_default(&pcfg);
    stepc_damon_detector_config_default(&dcfg);

    if (stepc_damon_detector_start(&detector, &mapping,
                                   &victim_pid, 1,
                                   candidate_pids, NCAND,
                                   &dcfg) != 0) {
        perror("[score] stepc_damon_detector_start");
        return 1;
    }
    detector_started = true;

    if (stepc_damon_detector_warmup(&detector,
                                    dcfg.startup_warmup_ms) != 0) {
        perror("[score] stepc_damon_detector_warmup");
        goto out;
    }

    /* Activity t0 is taken after DAMON warmup, immediately before the 1-s window. */
    for (i = 0; i < NCAND; ++i) {
        int rc = proc_activity_sample_now(st[i].pid, &st[i].before);
        if (rc != PROC_ACTIVITY_OK) {
            fprintf(stderr, "[score] sample(before) %s pid=%ld: %s\n",
                    st[i].label, (long)st[i].pid,
                    proc_activity_strerror(rc));
            goto out;
        }
    }

    if (stepc_damon_detector_collect(&detector) != 0) {
        perror("[score] stepc_damon_detector_collect");
        goto out;
    }

    for (i = 0; i < NCAND; ++i) {
        int rc = proc_activity_sample_now(st[i].pid, &st[i].after);
        if (rc != PROC_ACTIVITY_OK) {
            fprintf(stderr, "[score] sample(after) %s pid=%ld: %s\n",
                    st[i].label, (long)st[i].pid,
                    proc_activity_strerror(rc));
            goto out;
        }
        rc = proc_activity_compute_delta(&st[i].before,
                                         &st[i].after,
                                         &pcfg,
                                         &st[i].delta);
        if (rc != PROC_ACTIVITY_OK) {
            fprintf(stderr, "[score] delta %s pid=%ld: %s\n",
                    st[i].label, (long)st[i].pid,
                    proc_activity_strerror(rc));
            goto out;
        }

        memset(&st[i].activity, 0, sizeof(st[i].activity));
        st[i].activity.pid = st[i].pid;
        st[i].activity.delta = st[i].delta;
        /* Current Step-C activity-prefilter ranking definition. */
        st[i].activity.screening_score =
            max3(st[i].delta.cpu_score,
                 st[i].delta.fault_score,
                 st[i].delta.rss_score);
        st[i].activity.legacy_total_score = st[i].delta.total_score;
    }

    {
        struct activity_prefilter_candidate acts[NCAND];
        for (i = 0; i < NCAND; ++i)
            acts[i] = st[i].activity;

        if (stepc_damon_detector_build_candidates(&detector,
                                                  acts, NCAND,
                                                  out, NCAND,
                                                  &out_count) != 0) {
            perror("[score] stepc_damon_detector_build_candidates");
            goto out;
        }
    }

    if (out_count != NCAND) {
        fprintf(stderr,
                "[score] expected %d outputs but got %zu; "
                "refusing incomplete ranking\n",
                NCAND, out_count);
        goto out;
    }

    /* Stop DAMON before reporting success to the orchestration script. */
    if (stepc_damon_detector_stop(&detector) != 0) {
        perror("[score] stepc_damon_detector_stop");
        detector_started = false;
        goto out;
    }
    detector_started = false;

    printf("SCORE_META,window_ms=%u,warmup_ms=%u\n",
           dcfg.window_ms, dcfg.startup_warmup_ms);
    printf("SCORE_HEADER,label,pid,activity,cpu_score,fault_score,rss_score,"
           "legacy_total,cpu_ratio,faults_per_sec,rss_mib,active,overlap,fused\n");

    for (i = 0; i < NCAND; ++i) {
        const struct interference_candidate *c =
            find_output(out, out_count, st[i].pid);
        if (!c) {
            fprintf(stderr, "[score] missing output for %s pid=%ld\n",
                    st[i].label, (long)st[i].pid);
            return 1;
        }

        printf("SCORE,%s,%ld,%.9f,%.9f,%.9f,%.9f,%.9f,"
               "%.9f,%.9f,%.9f,%d,%.9f,%.9f\n",
               st[i].label,
               (long)st[i].pid,
               c->activity_score,
               c->cpu_score,
               c->fault_score,
               c->rss_score,
               st[i].delta.total_score,
               st[i].delta.cpu_ratio,
               st[i].delta.faults_per_sec,
               st[i].delta.rss_mib,
               st[i].delta.active ? 1 : 0,
               c->overlap_score,
               c->final_score);
    }

    ret = 0;
out:
    if (detector_started)
        (void)stepc_damon_detector_stop(&detector);
    return ret;
}

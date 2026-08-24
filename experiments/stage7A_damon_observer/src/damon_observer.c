#define _GNU_SOURCE
#include "damon_observer.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t len;
    ssize_t written;

    if (fd < 0)
        return -1;

    len = (ssize_t)strlen(text);
    written = write(fd, text, (size_t)len);
    close(fd);
    if (written != len) {
        if (written >= 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int write_ulong(const char *path, unsigned long value)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%lu\n", value);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_text(path, buf);
}

static int pathf(char *buf, size_t cap, const char *fmt,
                 const char *root, unsigned int idx)
{
    int n = snprintf(buf, cap, fmt, root, idx);
    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

void damon_observer_config_default(damon_observer_config_t *cfg)
{
    if (!cfg)
        return;
    cfg->sample_us = 5000;
    cfg->aggr_us = 100000;
    cfg->update_us = 1000000;
    cfg->min_regions = 128;
    cfg->max_regions = 1000;
    cfg->admin_root = "/sys/kernel/mm/damon/admin";
    cfg->trace_root = "/sys/kernel/tracing";
}

int damon_observer_init(damon_observer_t *obs,
                        const damon_observer_config_t *cfg,
                        const pid_t *pids,
                        size_t nr_pids)
{
    size_t i;

    if (!obs || !cfg || !pids || nr_pids == 0 ||
        nr_pids > DAMON_OBSERVER_MAX_TARGETS) {
        errno = EINVAL;
        return -1;
    }

    memset(obs, 0, sizeof(*obs));
    obs->cfg = *cfg;
    obs->trace_fd = -1;
    obs->nr_pids = nr_pids;
    for (i = 0; i < nr_pids; i++) {
        if (pids[i] <= 0) {
            errno = EINVAL;
            return -1;
        }
        obs->pids[i] = pids[i];
    }
    return 0;
}

static int configure_sysfs(damon_observer_t *obs)
{
    char path[512];
    size_t i;

    if (snprintf(path, sizeof(path), "%s/kdamonds/nr_kdamonds",
                 obs->cfg.admin_root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (write_ulong(path, 0) < 0 || write_ulong(path, 1) < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/kdamonds/0/contexts/nr_contexts",
                 obs->cfg.admin_root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (write_ulong(path, 1) < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/kdamonds/0/contexts/0/operations",
                 obs->cfg.admin_root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (write_text(path, "vaddr\n") < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/kdamonds/0/contexts/0/targets/nr_targets",
                 obs->cfg.admin_root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (write_ulong(path, (unsigned long)obs->nr_pids) < 0)
        return -1;

    for (i = 0; i < obs->nr_pids; i++) {
        if (pathf(path, sizeof(path),
                  "%s/kdamonds/0/contexts/0/targets/%u/pid_target",
                  obs->cfg.admin_root, (unsigned int)i) < 0)
            return -1;
        if (write_ulong(path, (unsigned long)obs->pids[i]) < 0)
            return -1;
    }

#define WRITE_ATTR(rel, val) do { \
    if (snprintf(path, sizeof(path), "%s/" rel, obs->cfg.admin_root) >= (int)sizeof(path)) { \
        errno = ENAMETOOLONG; \
        return -1; \
    } \
    if (write_ulong(path, (unsigned long)(val)) < 0) \
        return -1; \
} while (0)

    WRITE_ATTR("kdamonds/0/contexts/0/monitoring_attrs/intervals/sample_us",
               obs->cfg.sample_us);
    WRITE_ATTR("kdamonds/0/contexts/0/monitoring_attrs/intervals/aggr_us",
               obs->cfg.aggr_us);
    WRITE_ATTR("kdamonds/0/contexts/0/monitoring_attrs/intervals/update_us",
               obs->cfg.update_us);
    WRITE_ATTR("kdamonds/0/contexts/0/monitoring_attrs/nr_regions/min",
               obs->cfg.min_regions);
    WRITE_ATTR("kdamonds/0/contexts/0/monitoring_attrs/nr_regions/max",
               obs->cfg.max_regions);
#undef WRITE_ATTR

    return 0;
}

int damon_observer_start(damon_observer_t *obs)
{
    char path[512];

    if (!obs || obs->running) {
        errno = EINVAL;
        return -1;
    }

    if (configure_sysfs(obs) < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/events/damon/damon_aggregated/enable",
                 obs->cfg.trace_root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (write_text(path, "1\n") < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/trace_pipe", obs->cfg.trace_root) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    obs->trace_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (obs->trace_fd < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/kdamonds/0/state",
                 obs->cfg.admin_root) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        close(obs->trace_fd);
        obs->trace_fd = -1;
        return -1;
    }
    if (write_text(path, "on\n") < 0) {
        close(obs->trace_fd);
        obs->trace_fd = -1;
        return -1;
    }

    obs->running = 1;
    obs->read_len = 0;
    return 0;
}

int damon_observer_parse_trace_line(const char *line,
                                    const pid_t *pids,
                                    size_t nr_pids,
                                    damon_region_sample_t *out)
{
    const char *p;
    unsigned int target_id, nr_regions, nr_accesses, age;
    unsigned long long start, end;
    int matched;

    if (!line || !out)
        return 0;

    p = strstr(line, "damon_aggregated:");
    if (!p)
        return 0;

    matched = sscanf(p,
        "damon_aggregated: target_id=%u nr_regions=%u %llu-%llu: %u %u",
        &target_id, &nr_regions, &start, &end, &nr_accesses, &age);
    if (matched != 6)
        return 0;

    memset(out, 0, sizeof(*out));
    out->target_id = target_id;
    out->pid = (target_id < nr_pids && pids) ? pids[target_id] : (pid_t)-1;
    out->nr_regions = nr_regions;
    out->start = (uint64_t)start;
    out->end = (uint64_t)end;
    out->nr_accesses = nr_accesses;
    out->age = age;
    return 1;
}

static int consume_lines(damon_observer_t *obs,
                         damon_region_callback_t cb,
                         void *user_data)
{
    size_t start = 0;
    size_t i;
    int delivered = 0;

    for (i = 0; i < obs->read_len; i++) {
        if (obs->read_buf[i] == '\n') {
            damon_region_sample_t sample;
            obs->read_buf[i] = '\0';
            if (damon_observer_parse_trace_line(obs->read_buf + start,
                                                obs->pids,
                                                obs->nr_pids,
                                                &sample)) {
                if (cb)
                    cb(&sample, user_data);
                delivered++;
            }
            start = i + 1;
        }
    }

    if (start > 0) {
        size_t remain = obs->read_len - start;
        memmove(obs->read_buf, obs->read_buf + start, remain);
        obs->read_len = remain;
    }
    return delivered;
}

int damon_observer_poll(damon_observer_t *obs,
                        int timeout_ms,
                        damon_region_callback_t cb,
                        void *user_data)
{
    struct pollfd pfd;
    ssize_t n;
    int rc;
    int delivered = 0;

    if (!obs || !obs->running || obs->trace_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    pfd.fd = obs->trace_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0)
        return -1;
    if (rc == 0)
        return 0;

    if (!(pfd.revents & (POLLIN | POLLPRI)))
        return 0;

    if (obs->read_len == sizeof(obs->read_buf)) {
        obs->read_len = 0;
        errno = ENOBUFS;
        return -1;
    }

    n = read(obs->trace_fd,
             obs->read_buf + obs->read_len,
             sizeof(obs->read_buf) - obs->read_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        return -1;
    }
    if (n == 0)
        return 0;

    obs->read_len += (size_t)n;
    delivered += consume_lines(obs, cb, user_data);
    return delivered;
}

int damon_observer_stop(damon_observer_t *obs)
{
    char path[512];
    int saved_errno = 0;

    if (!obs)
        return -1;

    if (obs->running) {
        if (snprintf(path, sizeof(path), "%s/kdamonds/0/state",
                     obs->cfg.admin_root) < (int)sizeof(path)) {
            if (write_text(path, "off\n") < 0 && saved_errno == 0)
                saved_errno = errno;
        }

        if (snprintf(path, sizeof(path),
                     "%s/events/damon/damon_aggregated/enable",
                     obs->cfg.trace_root) < (int)sizeof(path)) {
            if (write_text(path, "0\n") < 0 && saved_errno == 0)
                saved_errno = errno;
        }
    }

    if (obs->trace_fd >= 0) {
        close(obs->trace_fd);
        obs->trace_fd = -1;
    }
    obs->running = 0;
    obs->read_len = 0;

    if (saved_errno) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

void damon_observer_destroy(damon_observer_t *obs)
{
    if (!obs)
        return;
    (void)damon_observer_stop(obs);
    memset(obs, 0, sizeof(*obs));
    obs->trace_fd = -1;
}


#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "damon_observer.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DAMON_ROOT "/sys/kernel/mm/damon/admin"
#define KDAMOND0 DAMON_ROOT "/kdamonds/0"
#define CTX0 KDAMOND0 "/contexts/0"
#define TRACE_EVENT_ENABLE "/sys/kernel/tracing/events/damon/damon_aggregated/enable"
#define TRACE_PIPE "/sys/kernel/tracing/trace_pipe"
#define TRACE_CLOCK "/sys/kernel/tracing/trace_clock"
#define TRACE_BUFFER "/sys/kernel/tracing/trace"

static int write_text(const char *path, const char *text)
{
    int fd;
    size_t len;
    ssize_t n;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    len = strlen(text);
    n = write(fd, text, len);
    if (n != (ssize_t)len) {
        int saved = n < 0 ? errno : EIO;
        close(fd);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0)
        return -1;
    return 0;
}


static int read_active_trace_clock(char *out, size_t out_size)
{
    int fd;
    char buf[512];
    ssize_t n;
    char *lb;
    char *rb;
    size_t len;

    if (out == NULL || out_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    fd = open(TRACE_CLOCK, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1U);
    if (n < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (close(fd) < 0)
        return -1;
    if (n == 0) {
        errno = EIO;
        return -1;
    }
    buf[n] = '\0';

    lb = strchr(buf, '[');
    rb = lb != NULL ? strchr(lb + 1, ']') : NULL;
    if (lb == NULL || rb == NULL || rb <= lb + 1) {
        errno = EPROTO;
        return -1;
    }

    len = (size_t)(rb - (lb + 1));
    if (len + 1U > out_size) {
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(out, lb + 1, len);
    out[len] = '\0';
    return 0;
}

static int select_monotonic_trace_clock(damon_observer_t *obs)
{
    char active[32];

    if (obs == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (read_active_trace_clock(obs->trace_clock_previous,
                                sizeof(obs->trace_clock_previous)) < 0)
        return -1;

    if (strcmp(obs->trace_clock_previous, "mono") != 0) {
        if (write_text(TRACE_CLOCK, "mono") < 0)
            return -1;
        obs->trace_clock_changed = 1;
    }

    if (read_active_trace_clock(active, sizeof(active)) < 0)
        goto fail_restore;
    if (strcmp(active, "mono") != 0) {
        errno = EIO;
        goto fail_restore;
    }
    snprintf(obs->trace_clock_name, sizeof(obs->trace_clock_name), "%s", active);
    return 0;

fail_restore:
    {
        int saved = errno;
        if (obs->trace_clock_changed) {
            (void)write_text(TRACE_CLOCK, obs->trace_clock_previous);
            obs->trace_clock_changed = 0;
        }
        obs->trace_clock_name[0] = '\0';
        errno = saved;
        return -1;
    }
}

static int restore_trace_clock(damon_observer_t *obs)
{
    if (obs == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!obs->trace_clock_changed)
        return 0;
    if (write_text(TRACE_CLOCK, obs->trace_clock_previous) < 0)
        return -1;
    obs->trace_clock_changed = 0;
    return 0;
}

static int write_ulong(const char *path, unsigned long v)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%lu", v);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_text(path, buf);
}

static int write_uint(const char *path, unsigned int v)
{
    return write_ulong(path, (unsigned long)v);
}

static int write_pid_path(const char *path, pid_t pid)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%d", (int)pid);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_text(path, buf);
}

static int configure_sysfs(damon_observer_t *obs)
{
    char path[256];
    size_t i;

    /* Best effort: an old instance may already be off. */
    (void)write_text(KDAMOND0 "/state", "off");

    if (write_text(DAMON_ROOT "/kdamonds/nr_kdamonds", "0") < 0)
        return -1;
    if (write_text(DAMON_ROOT "/kdamonds/nr_kdamonds", "1") < 0)
        return -1;
    if (write_text(KDAMOND0 "/contexts/nr_contexts", "1") < 0)
        return -1;
    if (write_text(CTX0 "/operations", "vaddr") < 0)
        return -1;

    if (snprintf(path, sizeof(path), "%s/targets/nr_targets", CTX0) >= (int)sizeof(path)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (write_ulong(path, (unsigned long)obs->nr_targets) < 0)
        return -1;

    for (i = 0; i < obs->nr_targets; ++i) {
        if (snprintf(path, sizeof(path), "%s/targets/%zu/pid_target", CTX0, i) >= (int)sizeof(path)) {
            errno = EOVERFLOW;
            return -1;
        }
        if (write_pid_path(path, obs->pids[i]) < 0)
            return -1;
    }

#define WRITE_ATTR(_suffix, _value, _fn) do { \
    if (snprintf(path, sizeof(path), "%s/%s", CTX0, (_suffix)) >= (int)sizeof(path)) { \
        errno = EOVERFLOW; \
        return -1; \
    } \
    if ((_fn)(path, (_value)) < 0) \
        return -1; \
} while (0)

    WRITE_ATTR("monitoring_attrs/intervals/sample_us", obs->cfg.sample_us, write_ulong);
    WRITE_ATTR("monitoring_attrs/intervals/aggr_us", obs->cfg.aggr_us, write_ulong);
    WRITE_ATTR("monitoring_attrs/intervals/update_us", obs->cfg.update_us, write_ulong);
    WRITE_ATTR("monitoring_attrs/nr_regions/min", obs->cfg.min_regions, write_uint);
    WRITE_ATTR("monitoring_attrs/nr_regions/max", obs->cfg.max_regions, write_uint);
#undef WRITE_ATTR

    return 0;
}

void damon_observer_config_default(damon_observer_config_t *cfg)
{
    if (cfg == NULL)
        return;
    cfg->sample_us = 5000UL;
    cfg->aggr_us = 100000UL;
    cfg->update_us = 1000000UL;
    cfg->min_regions = 128U;
    cfg->max_regions = 1000U;
}

int damon_observer_init(damon_observer_t *obs,
                        const damon_observer_config_t *cfg,
                        const pid_t *pids,
                        size_t nr_targets)
{
    size_t i;

    if (obs == NULL || cfg == NULL || pids == NULL || nr_targets == 0U ||
        nr_targets > DAMON_OBSERVER_MAX_TARGETS) {
        errno = EINVAL;
        return -1;
    }

    memset(obs, 0, sizeof(*obs));
    obs->trace_fd = -1;
    obs->cfg = *cfg;
    obs->nr_targets = nr_targets;
    for (i = 0; i < nr_targets; ++i) {
        if (pids[i] <= 0) {
            errno = EINVAL;
            return -1;
        }
        obs->pids[i] = pids[i];
    }

    return configure_sysfs(obs);
}

int damon_observer_start(damon_observer_t *obs)
{
    if (obs == NULL || obs->nr_targets == 0U) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Real-time windowing requires a globally monotonic trace time base.
     * Force ftrace's clock to mono for this experiment and restore the
     * previous selection on shutdown.
     */
    if (select_monotonic_trace_clock(obs) < 0)
        return -1;

    /* Start from a clean DAMON trace session; stale events would poison sync. */
    (void)write_text(TRACE_EVENT_ENABLE, "0");
    if (write_text(TRACE_BUFFER, "\n") < 0)
        goto fail_clock;

    obs->trace_fd = open(TRACE_PIPE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (obs->trace_fd < 0)
        goto fail_clock;

    if (write_text(TRACE_EVENT_ENABLE, "1") < 0)
        goto fail;
    if (write_text(KDAMOND0 "/state", "on") < 0)
        goto fail_disable;

    obs->started = 1;
    return 0;

fail_disable:
    (void)write_text(TRACE_EVENT_ENABLE, "0");
fail:
    {
        int saved = errno;
        close(obs->trace_fd);
        obs->trace_fd = -1;
        (void)restore_trace_clock(obs);
        errno = saved;
        return -1;
    }
fail_clock:
    {
        int saved = errno;
        (void)restore_trace_clock(obs);
        errno = saved;
        return -1;
    }
}

static int parse_timestamp_ns(const char *begin, const char *end, uint64_t *out)
{
    const char *dot;
    uint64_t sec = 0U;
    uint64_t frac = 0U;
    unsigned int digits = 0U;
    const char *p;

    if (begin == NULL || end == NULL || out == NULL || begin >= end)
        return -1;

    dot = memchr(begin, '.', (size_t)(end - begin));
    if (dot == NULL)
        dot = end;

    for (p = begin; p < dot; ++p) {
        if (!isdigit((unsigned char)*p))
            return -1;
        if (sec > UINT64_MAX / 10U)
            return -1;
        sec = sec * 10U + (uint64_t)(*p - '0');
    }

    if (dot < end) {
        for (p = dot + 1; p < end; ++p) {
            if (!isdigit((unsigned char)*p))
                return -1;
            if (digits < 9U) {
                frac = frac * 10U + (uint64_t)(*p - '0');
                ++digits;
            }
        }
    }
    while (digits < 9U) {
        frac *= 10U;
        ++digits;
    }
    if (sec > UINT64_MAX / 1000000000ULL)
        return -1;
    *out = sec * 1000000000ULL + frac;
    return 0;
}

static int parse_line(damon_observer_t *obs,
                      char *line,
                      damon_region_sample_t *s)
{
    const char *marker;
    const char *ts_end;
    const char *ts_begin;
    const char *payload;
    unsigned int target_id, nr_regions, nr_accesses, age;
    unsigned long long start, end;
    uint64_t ts_ns;
    int matched;

    marker = strstr(line, ": damon_aggregated:");
    if (marker == NULL)
        return 0;

    ts_end = marker;
    ts_begin = ts_end;
    while (ts_begin > line && !isspace((unsigned char)ts_begin[-1]))
        --ts_begin;
    if (parse_timestamp_ns(ts_begin, ts_end, &ts_ns) != 0)
        return -1;

    payload = marker + 2; /* skip ': ' */
    matched = sscanf(payload,
                     "damon_aggregated: target_id=%u nr_regions=%u %llu-%llu: %u %u",
                     &target_id, &nr_regions, &start, &end, &nr_accesses, &age);
    if (matched != 6)
        return -1;
    if (target_id >= obs->nr_targets)
        return -1;

    memset(s, 0, sizeof(*s));
    s->target_id = target_id;
    s->pid = obs->pids[target_id];
    s->nr_regions = nr_regions;
    s->start = (uint64_t)start;
    s->end = (uint64_t)end;
    s->nr_accesses = nr_accesses;
    s->age = age;
    s->trace_ts_ns = ts_ns;
    return 1;
}

int damon_observer_parse_trace_line_for_test(const char *line,
                                              const pid_t *pids,
                                              size_t nr_targets,
                                              damon_region_sample_t *sample)
{
    damon_observer_t obs;
    char buf[2048];
    size_t i;
    if (line == NULL || pids == NULL || sample == NULL ||
        nr_targets == 0U || nr_targets > DAMON_OBSERVER_MAX_TARGETS) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(line) >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(&obs, 0, sizeof(obs));
    obs.nr_targets = nr_targets;
    for (i = 0U; i < nr_targets; ++i)
        obs.pids[i] = pids[i];
    strcpy(buf, line);
    return parse_line(&obs, buf, sample);
}

static int consume_lines(damon_observer_t *obs,
                         damon_observer_callback_t cb,
                         void *user_data)
{
    size_t start = 0U;
    size_t i;

    for (i = 0U; i < obs->readbuf_used; ++i) {
        if (obs->readbuf[i] == '\n') {
            damon_region_sample_t s;
            int rc;
            obs->readbuf[i] = '\0';
            ++obs->trace_lines;
            rc = parse_line(obs, obs->readbuf + start, &s);
            if (rc > 0) {
                ++obs->matched_lines;
                if (cb != NULL)
                    cb(&s, user_data);
            } else if (rc < 0) {
                ++obs->parse_fail_lines;
            }
            start = i + 1U;
        }
    }

    if (start != 0U) {
        size_t remain = obs->readbuf_used - start;
        memmove(obs->readbuf, obs->readbuf + start, remain);
        obs->readbuf_used = remain;
    }
    return 0;
}

int damon_observer_poll(damon_observer_t *obs,
                        int timeout_ms,
                        damon_observer_callback_t cb,
                        void *user_data)
{
    struct pollfd pfd;
    ssize_t n;
    int prc;

    if (obs == NULL || obs->trace_fd < 0) {
        errno = EINVAL;
        return -1;
    }

    pfd.fd = obs->trace_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    prc = poll(&pfd, 1, timeout_ms);
    if (prc < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }
    if (prc == 0)
        return 0;
    if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        errno = EIO;
        return -1;
    }

    if (obs->readbuf_used == sizeof(obs->readbuf)) {
        obs->readbuf_used = 0U;
        ++obs->parse_fail_lines;
    }

    n = read(obs->trace_fd,
             obs->readbuf + obs->readbuf_used,
             sizeof(obs->readbuf) - obs->readbuf_used);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        return -1;
    }
    if (n == 0)
        return 0;

    obs->readbuf_used += (size_t)n;
    return consume_lines(obs, cb, user_data);
}

int damon_observer_stop(damon_observer_t *obs)
{
    int rc = 0;
    int saved = 0;

    if (obs == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (write_text(KDAMOND0 "/state", "off") < 0) {
        rc = -1;
        saved = errno;
    }
    (void)write_text(TRACE_EVENT_ENABLE, "0");
    obs->started = 0;

    if (restore_trace_clock(obs) < 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }

    if (rc < 0)
        errno = saved;
    return rc;
}

void damon_observer_destroy(damon_observer_t *obs)
{
    if (obs == NULL)
        return;
    (void)write_text(TRACE_EVENT_ENABLE, "0");
    (void)restore_trace_clock(obs);
    if (obs->trace_fd >= 0)
        close(obs->trace_fd);
    obs->trace_fd = -1;
    obs->started = 0;
}

void damon_observer_get_parse_stats(const damon_observer_t *obs,
                                    uint64_t *trace_lines,
                                    uint64_t *matched_lines,
                                    uint64_t *parse_fail_lines)
{
    if (trace_lines != NULL)
        *trace_lines = obs != NULL ? obs->trace_lines : 0U;
    if (matched_lines != NULL)
        *matched_lines = obs != NULL ? obs->matched_lines : 0U;
    if (parse_fail_lines != NULL)
        *parse_fail_lines = obs != NULL ? obs->parse_fail_lines : 0U;
}

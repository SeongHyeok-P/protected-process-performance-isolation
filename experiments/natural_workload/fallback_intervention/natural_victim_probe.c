#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static volatile uint32_t g_sink = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static uint64_t rng_next(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static uint64_t ts_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static int now_raw(struct timespec *ts)
{
    return clock_gettime(CLOCK_MONOTONIC_RAW, ts);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <size_MiB> <epoch_ms> [seed]\n"
            "Example: %s 64 1000 20261001\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    unsigned long size_mib;
    unsigned long epoch_ms;
    unsigned long long seed_in = 20261001ULL;
    size_t count, i;
    uint32_t *next = NULL;
    uint32_t *perm = NULL;
    uint32_t idx = 0;
    uint64_t rng_state;
    uint64_t epoch = 0;
    struct sigaction sa;

    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 2;
    }

    char *end = NULL;
    errno = 0;
    size_mib = strtoul(argv[1], &end, 10);
    if (errno || end == argv[1] || *end != '\0' || size_mib == 0) {
        fprintf(stderr, "Invalid size_MiB\n");
        return 2;
    }

    end = NULL;
    errno = 0;
    epoch_ms = strtoul(argv[2], &end, 10);
    if (errno || end == argv[2] || *end != '\0' || epoch_ms == 0) {
        fprintf(stderr, "Invalid epoch_ms\n");
        return 2;
    }

    if (argc == 4) {
        end = NULL;
        errno = 0;
        seed_in = strtoull(argv[3], &end, 0);
        if (errno || end == argv[3] || *end != '\0' || seed_in == 0) {
            fprintf(stderr, "Invalid seed\n");
            return 2;
        }
    }

    if ((size_t)size_mib > SIZE_MAX / (1024ULL * 1024ULL)) {
        fprintf(stderr, "size_MiB too large\n");
        return 2;
    }

    const size_t bytes = (size_t)size_mib * 1024ULL * 1024ULL;
    count = bytes / sizeof(uint32_t);
    if (count < 2 || count > UINT32_MAX) {
        fprintf(stderr, "Working set must contain 2..UINT32_MAX entries\n");
        return 2;
    }

    next = aligned_alloc(64, count * sizeof(*next));
    perm = malloc(count * sizeof(*perm));
    if (!next || !perm) {
        perror("allocation");
        free(next);
        free(perm);
        return 1;
    }

    for (i = 0; i < count; ++i)
        perm[i] = (uint32_t)i;

    rng_state = (uint64_t)seed_in;
    for (i = count - 1; i > 0; --i) {
        size_t j = (size_t)(rng_next(&rng_state) % (i + 1));
        uint32_t tmp = perm[i];
        perm[i] = perm[j];
        perm[j] = tmp;
    }

    for (i = 0; i + 1 < count; ++i)
        next[perm[i]] = perm[i + 1];
    next[perm[count - 1]] = perm[0];

    idx = perm[0];
    free(perm);
    perm = NULL;

    /* Warm the whole dependent chain once before announcing READY. */
    for (i = 0; i < count; ++i)
        idx = next[idx];
    g_sink = idx;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("READY,pid=%ld,size_mib=%lu,epoch_ms=%lu,seed=%llu\n",
           (long)getpid(), size_mib, epoch_ms, seed_in);

    while (!g_stop) {
        struct timespec start, now;
        uint64_t reads = 0;
        uint64_t target_ns;
        const uint64_t batch = 4096;

        if (now_raw(&start) != 0) {
            perror("clock_gettime");
            break;
        }
        target_ns = ts_ns(&start) + (uint64_t)epoch_ms * 1000000ULL;

        do {
            uint64_t k;
            for (k = 0; k < batch; ++k)
                idx = next[idx];
            reads += batch;
            if (now_raw(&now) != 0) {
                perror("clock_gettime");
                g_stop = 1;
                break;
            }
        } while (!g_stop && ts_ns(&now) < target_ns);

        if (reads == 0)
            continue;
        if (now_raw(&now) != 0)
            break;

        double elapsed =
            (double)(ts_ns(&now) - ts_ns(&start)) / 1000000000.0;
        double ns_per_read = elapsed * 1e9 / (double)reads;
        ++epoch;

        printf("VICTIM_SAMPLE,epoch=%" PRIu64
               ",reads=%" PRIu64
               ",elapsed_sec=%.9f,ns_per_read=%.6f\n",
               epoch, reads, elapsed, ns_per_read);
    }

    g_sink = idx;
    fprintf(stderr, "[victim] exit pid=%ld sink=%u\n",
            (long)getpid(), (unsigned)g_sink);
    free(next);
    return 0;
}

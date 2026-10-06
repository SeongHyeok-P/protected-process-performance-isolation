#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile uint32_t sink_index = 0;

static uint64_t rng_next(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

static double elapsed_sec(const struct timespec *start,
                          const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    char *endptr = NULL;
    unsigned long size_mib;
    unsigned long run_seconds;
    size_t bytes;
    size_t elements;
    uint32_t *next = NULL;
    uint32_t *order = NULL;
    uint32_t index;
    uint64_t rng_state = 0x123456789abcdefULL;
    volatile uint32_t *vnext;
    struct timespec start;
    struct timespec now;
    uint64_t total_reads = 0;
    const uint64_t chunk_reads = 100000;
    double sec;
    double ns_per_read;
    long page_size;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <size_MiB> <run_seconds>\n", argv[0]);
        return 1;
    }

    errno = 0;
    size_mib = strtoul(argv[1], &endptr, 10);
    if (errno != 0 || endptr == argv[1] || *endptr != '\0' || size_mib == 0) {
        fprintf(stderr, "invalid size_MiB\n");
        return 1;
    }

    endptr = NULL;
    errno = 0;
    run_seconds = strtoul(argv[2], &endptr, 10);
    if (errno != 0 || endptr == argv[2] || *endptr != '\0' || run_seconds == 0) {
        fprintf(stderr, "invalid run_seconds\n");
        return 1;
    }

    if (size_mib > SIZE_MAX / (1024ULL * 1024ULL)) {
        fprintf(stderr, "size overflow\n");
        return 1;
    }

    bytes = (size_t)size_mib * 1024ULL * 1024ULL;
    elements = bytes / sizeof(uint32_t);
    if (elements < 2 || elements > UINT32_MAX) {
        fprintf(stderr, "unsupported element count\n");
        return 1;
    }

    if (posix_memalign((void **)&next, 64, elements * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "next allocation failed\n");
        return 1;
    }
    if (posix_memalign((void **)&order, 64, elements * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "order allocation failed\n");
        free(next);
        return 1;
    }

    for (size_t i = 0; i < elements; ++i)
        order[i] = (uint32_t)i;

    for (size_t i = elements - 1; i > 0; --i) {
        size_t j = (size_t)(rng_next(&rng_state) % (i + 1));
        uint32_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }

    for (size_t i = 0; i + 1 < elements; ++i)
        next[order[i]] = order[i + 1];
    next[order[elements - 1]] = order[0];
    index = order[0];

    free(order);
    order = NULL;

    /* Touch the complete permutation once before the controlled run. */
    for (size_t i = 0; i < elements; ++i)
        index = next[index];
    sink_index = index;

    page_size = sysconf(_SC_PAGESIZE);

    /*
     * Diagnostic metadata uses CLOCK_MONOTONIC so it is directly comparable
     * with damon_class_observer_cli_diag.  The latency measurement itself
     * intentionally keeps the original CLOCK_MONOTONIC_RAW clock.
     */
    fprintf(stderr,
            "READY pid=%d size=%luMiB page_size=%ld "
            "next_start=0x%016" PRIxPTR " next_end=0x%016" PRIxPTR " "
            "ready_mono_ns=%" PRIu64 "\n",
            getpid(), size_mib, page_size,
            (uintptr_t)next, (uintptr_t)next + bytes,
            monotonic_ns());
    fflush(stderr);

    if (raise(SIGSTOP) != 0) {
        perror("raise(SIGSTOP)");
        free(next);
        return 1;
    }

    fprintf(stderr,
            "RESUMED pid=%d resumed_mono_ns=%" PRIu64 "\n",
            getpid(), monotonic_ns());
    fflush(stderr);

    vnext = (volatile uint32_t *)next;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
        perror("clock_gettime");
        free(next);
        return 1;
    }

    for (;;) {
        for (uint64_t i = 0; i < chunk_reads; ++i)
            index = vnext[index];

        total_reads += chunk_reads;

        if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
            perror("clock_gettime");
            free(next);
            return 1;
        }

        if (elapsed_sec(&start, &now) >= (double)run_seconds)
            break;
    }

    sink_index = index;
    sec = elapsed_sec(&start, &now);
    ns_per_read = (sec * 1000000000.0) / (double)total_reads;

    fprintf(stderr,
            "END pid=%d end_mono_ns=%" PRIu64 " total_reads=%" PRIu64 "\n",
            getpid(), monotonic_ns(), total_reads);
    fflush(stderr);

    printf("size_mib,%lu\n", size_mib);
    printf("elapsed_sec,%.6f\n", sec);
    printf("total_reads,%" PRIu64 "\n", total_reads);
    printf("ns_per_read,%.3f\n", ns_per_read);

    free(next);
    return 0;
}

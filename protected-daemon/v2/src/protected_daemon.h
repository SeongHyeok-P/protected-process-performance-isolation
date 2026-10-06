/* Minimal shared event ABI used by process.c and protected_daemon.bpf.c. */
#ifndef PROTECTED_DAEMON_H
#define PROTECTED_DAEMON_H

#include <stdint.h>

#define TASK_COMM_LEN 16

enum event_type {
    EVENT_EXEC = 1,
    EVENT_FORK = 2,
    EVENT_EXIT = 3
};

struct event {
    int type;
    int pid;
    int ppid;
    int child_pid;
    uint32_t uid;
    char comm[TASK_COMM_LEN];
};

#endif

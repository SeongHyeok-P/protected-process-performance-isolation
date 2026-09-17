CC ?= gcc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?= -Isrc
LDLIBS ?= -lm

CORE_SRCS = \
 src/calibration.c src/dram_mapping.c src/class_projector.c \
 src/addr_translate.c src/bank_profile.c \
 src/proc_activity.c src/activity_prefilter.c \
 src/cgroup.c src/candidate_filter.c src/process.c \
 src/damon_multi_observer.c src/damon_profile_window.c \
 src/stepc_damon_detector.c src/stepc_runtime_adapter.c src/throttle_controller.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

.PHONY: all test clean
all: libstepc_integration.a

libstepc_integration.a: $(CORE_OBJS)
	ar rcs $@ $^

stepc_integration_selftest: tests/stepc_integration_selftest.c libstepc_integration.a
	$(CC) $(CFLAGS) $(CPPFLAGS) $< libstepc_integration.a $(LDLIBS) -o $@

test: stepc_integration_selftest
	./stepc_integration_selftest

clean:
	rm -f $(CORE_OBJS) libstepc_integration.a stepc_integration_selftest


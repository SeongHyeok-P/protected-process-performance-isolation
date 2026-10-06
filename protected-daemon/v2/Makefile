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

# Daemon build: v2 is the entry point; existing core and test targets remain.
CLANG ?= clang
BPFTOOL_SRC ?= $(abspath ../../libbpf-bootstrap/bpftool/src)
BPFTOOL ?= $(BUILD_DIR)/bpftool/bootstrap/bpftool
LIBBPF_SRC ?= $(abspath ../../libbpf-bootstrap/libbpf/src)
BUILD_DIR := $(CURDIR)/.build
LIBBPF_DIR := $(BUILD_DIR)/libbpf
LIBBPF_A := $(LIBBPF_DIR)/lib/libbpf.a
VMLINUX ?= /sys/kernel/btf/vmlinux
BPF_ARCH ?= $(shell uname -m | sed -e 's/x86_64/x86/' -e 's/aarch64/arm64/')
DAEMON_SRCS := src/protected_daemon.c src/policy.c src/daemon_runtime.c
DAEMON_OBJS := $(DAEMON_SRCS:.c=.o)
BPF_CFLAGS ?= -g -O2 -target bpf -D__TARGET_ARCH_$(BPF_ARCH)

.PHONY: daemon clean-daemon
all: daemon
daemon: protected_daemon

$(LIBBPF_A):
	$(MAKE) -C $(LIBBPF_SRC) BUILD_STATIC_ONLY=1 OBJDIR=$(LIBBPF_DIR)/obj DESTDIR=$(LIBBPF_DIR) PREFIX= LIBDIR=/lib INCLUDEDIR=/include install

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/bpftool/bootstrap/bpftool: | $(BUILD_DIR)
	$(MAKE) -C $(BPFTOOL_SRC) OUTPUT=$(BUILD_DIR)/bpftool/ bootstrap

$(BUILD_DIR)/vmlinux.h: $(BPFTOOL) | $(BUILD_DIR)
	$(BPFTOOL) btf dump file $(VMLINUX) format c > $@.tmp
	mv $@.tmp $@

$(BUILD_DIR)/protected_daemon.bpf.o: src/protected_daemon.bpf.c src/protected_daemon.h $(BUILD_DIR)/vmlinux.h $(LIBBPF_A)
	$(CLANG) $(BPF_CFLAGS) -Isrc -I$(BUILD_DIR) -I$(LIBBPF_DIR)/include -c $< -o $@

$(BUILD_DIR)/protected_daemon.skel.h: $(BUILD_DIR)/protected_daemon.bpf.o
	$(BPFTOOL) gen skeleton $< > $@.tmp
	mv $@.tmp $@

src/protected_daemon.o: $(BUILD_DIR)/protected_daemon.skel.h $(LIBBPF_A)
$(DAEMON_OBJS): CPPFLAGS += -I$(BUILD_DIR) -I$(LIBBPF_DIR)/include

protected_daemon: $(DAEMON_OBJS) libstepc_integration.a $(LIBBPF_A)
	$(CC) $(CFLAGS) $(DAEMON_OBJS) libstepc_integration.a $(LIBBPF_A) $(LDLIBS) -lelf -lz -o $@

# Dependency files ensure edits to headers rebuild their consumers.
$(CORE_OBJS) $(DAEMON_OBJS): CFLAGS += -MMD -MP
-include $(CORE_OBJS:.o=.d) $(DAEMON_OBJS:.o=.d)

clean: clean-daemon
clean-daemon:
	rm -f $(DAEMON_OBJS) $(CORE_OBJS:.o=.d) $(DAEMON_OBJS:.o=.d) protected_daemon stepc_nonblocking_test daemon_runtime_test
	rm -rf $(BUILD_DIR)

CC ?= gcc
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Wpedantic -Werror
V2_ROOT ?= $(abspath $(CURDIR)/../../../protected-daemon/v2)
CPPFLAGS ?= -I$(V2_ROOT)/src
LDLIBS ?= -lm
STEPCLIB := $(V2_ROOT)/libstepc_integration.a

.PHONY: all clean v2lib check
all: natural_victim_probe natural_score_probe

v2lib:
	$(MAKE) -C "$(V2_ROOT)" libstepc_integration.a

natural_victim_probe: natural_victim_probe.c
	$(CC) $(CFLAGS) $< -o $@

natural_score_probe: natural_score_probe.c v2lib
	$(CC) $(CFLAGS) $(CPPFLAGS) natural_score_probe.c "$(STEPCLIB)" $(LDLIBS) -o $@

check: all
	./natural_victim_probe 1 50 12345 >/tmp/natural_victim_probe.check.log 2>&1 & \
	pid=$$!; sleep 0.4; kill $$pid 2>/dev/null || true; wait $$pid 2>/dev/null || true; \
	grep -q '^READY,' /tmp/natural_victim_probe.check.log; \
	grep -q '^VICTIM_SAMPLE,' /tmp/natural_victim_probe.check.log

clean:
	rm -f natural_victim_probe natural_score_probe

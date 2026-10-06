# Optional test-only additions; does not replace your v2 Makefile.
stepc_nonblocking_test: tests/stepc_nonblocking_test.c libstepc_integration.a
	$(CC) $(CFLAGS) $(CPPFLAGS) $< libstepc_integration.a $(LDLIBS) -Wl,--wrap=clock_gettime -Wl,--wrap=damon_multi_observer_poll -Wl,--wrap=throttle_controller_apply_candidates -o $@
.PHONY: test-nonblocking
test-nonblocking: stepc_nonblocking_test
	./stepc_nonblocking_test

daemon_runtime_test: tests/daemon_runtime_test.c src/daemon_runtime.o src/policy.o libstepc_integration.a
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -Wl,--wrap=clock_gettime -Wl,--wrap=proc_activity_sample_now -Wl,--wrap=candidate_filter_classify_pid -Wl,--wrap=stepc_damon_detector_start -Wl,--wrap=stepc_damon_detector_stop -Wl,--wrap=stepc_damon_detector_collect -o $@
.PHONY: test-runtime
test-runtime: daemon_runtime_test
	./daemon_runtime_test

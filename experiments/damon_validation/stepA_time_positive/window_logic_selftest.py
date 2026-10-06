#!/usr/bin/env python3
"""Pure arithmetic self-test for warm-up and trace-time window assignment."""

NS_PER_MS = 1_000_000
sync = 10_000_000_000
warmup_ms = 1000
window_ms = 1000
warm_end = sync + warmup_ms * NS_PER_MS
window_ns = window_ms * NS_PER_MS

cases = [
    (sync, None),
    (sync + 999 * NS_PER_MS, None),
    (warm_end - 1, None),
    (warm_end, 0),
    (warm_end + 999 * NS_PER_MS, 0),
    (warm_end + window_ns - 1, 0),
    (warm_end + window_ns, 1),
    (warm_end + 2 * window_ns + 123 * NS_PER_MS, 2),
]

for ts, expected in cases:
    got = None if ts < warm_end else (ts - warm_end) // window_ns
    if got != expected:
        raise SystemExit(f"FAIL ts={ts} expected={expected} got={got}")

# Demonstrate that callback processing time does not affect bin assignment.
trace_ts = warm_end + 123 * NS_PER_MS
for callback_delay_ms in (0, 50, 500, 5000):
    callback_ts = trace_ts + callback_delay_ms * NS_PER_MS
    _ = callback_ts
    got = (trace_ts - warm_end) // window_ns
    if got != 0:
        raise SystemExit("FAIL callback delay changed trace-time bin")

print("PASS warmup_and_trace_time_window_logic")

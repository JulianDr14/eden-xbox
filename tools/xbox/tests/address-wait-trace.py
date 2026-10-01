# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression fixtures for arbiter wake attribution across host/core migration."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "frame_trace", Path(__file__).resolve().parents[1] / "analyze-frame-trace.py")
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)

events = [
    (1., 10, "address-wait-begin", 0x1234, 83),
    (1., 10, "address-wait-condition", 0xffffffff | (0xffffffff << 32), 83 | (2 << 32)),
    (1., 10, "address-wait-timeout", (1 << 64) - 1, 83),
    (10., 11, "address-wake", 0x9999, 83 | (99 << 32)),  # Wrong address.
    (20., 12, "address-wake", 0x1234, 83 | (125 << 32)),
    (20., 12, "guest-thread-ready", 83, 16),
    (22., 14, "guest-dispatch", 83, 2),
    (22.2, 14, "address-wait-end", 0, 83),
    (30., 14, "address-wait-begin", 0x1234, 83),
    (35., 15, "address-cancel", 0xea01, 83),
    (36., 16, "address-wait-end", 0xea01, 83),
    (40., 16, "address-wait-end", 0, 83),  # Started before capture: unknown.
    (50., 16, "address-wait-begin", 0x4321, 83),  # Ends beyond capture.
]
waits, unfinished = trace.address_waits(events)
assert len(waits) == 2 and unfinished == 1
assert waits[0]["signalling_guest"] == 125
assert waits[0]["observed"] == waits[0]["expected"] == -1
assert waits[0]["absolute_timeout_ticks"] == -1
assert waits[0]["wake_to_dispatch_ms"] == 2
assert waits[0]["wake_to_resume_ms"] == 2.2
assert waits[0]["begin_host"] != waits[0]["end_host"]
assert waits[1]["cancel_result"] == waits[1]["result"] == "0xea01"
assert "signalling_guest" not in waits[1]
assert trace.address_waits([]) == ([], 0)
print("Address-wait fixtures passed: signal, cancellation, migration, signed values and borders")


chain = [
    (10., 10, "address-wait-begin", 0x1234, 83),
    (15., 11, "guest-svc-live-pc", 0x810010, (79 << 8) | 52),
    (15., 11, "address-wait-begin", 0xabcd, 79),
    (25., 12, "address-wake", 0xabcd, 79 | (77 << 32)),
    (25.1, 11, "address-wait-end", 0, 79),
    (25.1, 11, "guest-svc-long", 10100, (79 << 8) | 52),
    (29., 11, "guest-run-long", 2000, (79 << 8) | 0),
    (29., 11, "guest-run-pc", 0x810020, (79 << 8) | 0),
    (30., 11, "guest-svc-live-pc", 0x810030, (79 << 8) | 53),
    (30., 11, "guest-svc-live-lr", 0x810040, (79 << 8) | 53),
    (30., 11, "address-signal-request", 0x1234, 79),
    (30., 11, "address-signal-args", 1 | (0xffffffff << 32), 79 | (1 << 32)),
    (30.1, 11, "address-wake", 0x1234, 83 | (79 << 32)),
    (30.2, 11, "address-signal-end", 0, 79),
    (30.3, 14, "address-wait-end", 0, 83),
]
result = trace.analyze(chain)
root = next(w for w in result["address_wait_longest"] if w["guest"] == 83)
assert root["signal"]["pc"] == "0x810030"
assert root["signal"]["lr"] == "0x810040"
assert root["signal"]["count"] == -1 and root["signal"]["signal_type"] == 1
assert root["signal"]["result"] == "0x0"
assert root["signaller_svc_overlap_ms"] == 10.1
assert root["signaller_long_run_overlap_ms"] == 2.
assert root["signaller_waits"][0]["pc"] == "0x810010"
assert next(w for w in result["address_wait_longest"] if w["guest"] == 79)["signalling_guest"] == 77
print("Signalling chain passed: 83 <- 79 <- 77, source PC/args/result and clipped overlap")

# Older guest-svc-pc/lr records came from a potentially stale saved context.
legacy = [(t, h, k.replace("guest-svc-live-", "guest-svc-"), a, b)
          for t, h, k, a, b in chain]
legacy_root = next(w for w in trace.analyze(legacy)["address_wait_longest"] if w["guest"] == 83)
assert "pc" not in legacy_root["signal"] and "lr" not in legacy_root["signal"]
assert "pc" not in legacy_root["signaller_waits"][0]
assert legacy_root["signal"]["signal_type"] == 1
print("Legacy saved-context PCs rejected; timing and signal arguments retained")


completion = [
    (0., 10, "fence-queued", 7, 1),
    (.1, 10, "fence-backend-tick", 17, 7),
    (.2, 20, "fence-dequeued", 7, 0),
    (1., 11, "nv-event-armed", 300, 2 | (99 << 32)),
    (2., 12, "sync-wait-begin", 1, 79),
    (2., 12, "sync-wait-object", 300, 79 << 32),
    (8., 20, "fence-gpu-wait-long", 7800, 17),
    (8., 20, "fence-wait-long", 7800, 7),
    (9., 20, "fence-flush-lock-long", 1000, 7),
    (10., 20, "fence-flush-long", 2000, 7),
    (10., 20, "fence-callbacks-begin", 7, 1),
    (10.5, 20, "nv-event-signal", 300, 2 | (99 << 32)),
    (11., 20, "sync-wake-object", 300, 79),
    (11.1, 20, "nv-event-signal-long", 600, 300),
    (11.2, 20, "fence-done", 7, 0),
    (11.5, 12, "sync-wait-end", 0, 79),
    (14., 10, "fence-queued", 7, 0),  # Object pointer reused after old lifetime.
    (15., 20, "fence-done", 7, 0),
    (16., 12, "sync-wait-begin", 1, 79),
    (16., 12, "sync-wait-object", 301, 79 << 32),
    (17., 20, "sync-wake-object", 301, 79),  # Different object cannot inherit NV link.
    (17.1, 12, "sync-wait-end", 0, 79),
]
waits, fences, unfinished = trace.completion_chains(completion)
assert len(waits) == 2 and len(fences) == 2 and unfinished == 0
linked = waits[0]["nv_signal"]
assert linked["syncpoint"] == 2 and linked["target"] == 99
assert linked["fence"]["tick"] == 17 and linked["fence"]["queued_ms"] == 0
assert linked["fence"]["done_ms"] == 11.2  # Later lifetime must not overwrite it.
assert linked["fence"]["phases_ms"]["fence-gpu-wait-long"] == 7.8
assert linked["fence"]["phases_ms"]["fence-flush-long"] == 2
assert linked["fence"]["phases_ms"]["fence-flush-lock-long"] == 1  # Nested, never sum.
assert waits[0]["wake_to_resume_ms"] == .5
assert "nv_signal" not in waits[1]
assert trace.completion_chains([]) == ([], [], 0)
print("Completion fixtures passed: object/syncpoint/fence identity, nested phases and reuse")

# A foreground GC wait can use the same tick before the async fence is dequeued.
# Tick equality proves the completion target, not that it is nested in WaitFence.
foreground = [(0., 10, "fence-queued", 7, 1),
              (0., 10, "fence-backend-tick", 17, 7),
              (20., 10, "fence-gpu-wait-long", 18000, 17),
              (25., 20, "fence-dequeued", 7, 0),
              (25.1, 20, "fence-done", 7, 0)]
_, rows, _ = trace.completion_chains(foreground)
assert "fence-gpu-wait-long" not in rows[0]["phases_ms"]
assert rows[0]["backend_waits_on_tick"][0]["elapsed_ms"] == 18
assert not rows[0]["backend_waits_on_tick"][0]["part_of_fence_wait"]
print("Foreground same-tick GC wait kept separate from async WaitFence")

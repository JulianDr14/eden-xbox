# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read T captures without modifying them; report correlations, not causal attribution."""
import argparse
import bisect
import collections
import json
from pathlib import Path
import re


def percentile(values, fraction):
    values = sorted(values)
    return round(values[int((len(values) - 1) * fraction)], 3) if values else None


def covered_ms(spans, start, end):
    """Union clipped spans: nested elapsed regions must never be added twice."""
    clipped = sorted((max(start, a), min(end, b)) for a, b in spans if a < end and b > start)
    total, until = 0.0, start
    for a, b in clipped:
        total += max(0.0, b - max(a, until))
        until = max(until, b)
    return round(total, 3)


def address_waits(events):
    """Actual enqueues, wake/cancel, scheduler dispatch and resumed waits across hosts."""
    pending, completed, svc_locations, signals = {}, [], {}, {}
    for t, host, kind, a, b in events:
        if kind in ("guest-svc-live-pc", "guest-svc-live-lr"):
            point = svc_locations.setdefault((b >> 8, b & 255), {})
            point["pc" if kind.endswith("-pc") else "lr"] = hex(a)
            point["at_ms"] = t
        elif kind == "address-signal-request":
            signals[b] = {"address": hex(a), "request_ms": t, "guest": b}
            point = svc_locations.get((b, 53), {})
            if point and t >= point["at_ms"]:
                signals[b].update({k: point[k] for k in ("pc", "lr") if k in point})
        elif kind == "address-signal-args" and (b & 0xffffffff) in signals:
            signed = lambda n: n - (1 << 32) if n & (1 << 31) else n
            signals[b & 0xffffffff].update(expected=signed(a & 0xffffffff),
                count=signed(a >> 32), signal_type=b >> 32)
        elif kind == "address-signal-end" and b in signals:
            signals.pop(b).update(result=hex(a), end_ms=t)
        elif kind == "address-wait-begin":
            pending[b] = {"guest": b, "address": hex(a), "begin_ms": t, "begin_host": host}
            point = svc_locations.get((b, 52), {})
            if point and t >= point["at_ms"]:
                pending[b].update({k: point[k] for k in ("pc", "lr") if k in point})
        elif kind == "address-wait-condition" and (b & 0xffffffff) in pending:
            signed = lambda n: n - (1 << 32) if n & (1 << 31) else n
            pending[b & 0xffffffff].update(observed=signed(a & 0xffffffff),
                expected=signed(a >> 32), arbitration_type=b >> 32)
        elif kind == "address-wait-timeout" and b in pending:
            pending[b]["absolute_timeout_ticks"] = a - (1 << 64) if a >> 63 else a
        elif kind == "address-wake" and (b & 0xffffffff) in pending:
            w = pending[b & 0xffffffff]
            if w["address"] == hex(a):
                source = b >> 32
                w.update(wake_ms=t, signalling_guest=source, wake_host=host)
                signal = signals.get(source)
                if signal and signal["address"] == w["address"]:
                    w["signal"] = signal
        elif kind == "address-cancel" and b in pending:
            pending[b].update(cancel_ms=t, cancel_result=hex(a))
        elif kind == "guest-thread-ready" and a in pending:
            pending[a].setdefault("ready_ms", t)
        elif kind == "guest-dispatch" and a in pending:
            if "wake_ms" in pending[a] or "cancel_ms" in pending[a]:
                pending[a].setdefault("dispatch_ms", t)
                pending[a].setdefault("resume_core", b)
        elif kind == "address-wait-end" and b in pending:
            w = pending.pop(b)
            w.update(end_ms=t, end_host=host, result=hex(a), elapsed_ms=round(t-w["begin_ms"], 3))
            if "wake_ms" in w:
                w["wake_to_resume_ms"] = round(t-w["wake_ms"], 3)
                if "dispatch_ms" in w:
                    w["wake_to_dispatch_ms"] = round(w["dispatch_ms"]-w["wake_ms"], 3)
            completed.append(w)
    return completed, len(pending)


def completion_chains(events):
    """Object identity proves the wake link; phase elapsed still includes host scheduling."""
    fences, completed, callbacks, ticks = {}, [], {}, {}
    armed, signals, pending, waits = {}, {}, {}, []
    backend_phases = {"fence-submit-wait-long", "fence-gpu-wait-long"}
    fence_phases = {"fence-wait-long", "fence-flush-long", "fence-flush-lock-long",
                    "fence-texture-flush-long", "fence-buffer-flush-long",
                    "fence-query-flush-long", "fence-callbacks-long"}
    for t, host, kind, a, b in events:
        if kind == "fence-queued":
            fences[a] = {"id": hex(a), "queued_ms": t, "operations": b, "phases_ms": {}}
        elif kind == "fence-backend-tick":
            fence = fences.setdefault(b, {"id": hex(b), "phases_ms": {}})
            fence["tick"] = a
            ticks[a] = fence
        elif kind == "fence-dequeued":
            fence = fences.setdefault(a, {"id": hex(a), "phases_ms": {}})
            fence.update(dequeued_ms=t, dequeued_host=host, stubbed=bool(b))
            if "queued_ms" in fence:
                fence["queue_to_dequeue_ms"] = round(t-fence["queued_ms"], 3)
        elif kind in fence_phases:
            fences.setdefault(b, {"id": hex(b), "phases_ms": {}})["phases_ms"][kind] = a/1000
        elif kind in backend_phases and b in ticks:
            fence = ticks[b]
            start = t-a/1000
            nested = host == fence.get("dequeued_host") and start >= fence.get("dequeued_ms", t)-.002
            fence.setdefault("backend_waits_on_tick", []).append({
                "phase": kind, "host": host, "start_ms": start, "end_ms": t,
                "elapsed_ms": a/1000, "part_of_fence_wait": nested})
            if nested:
                fence["phases_ms"][kind] = a/1000
        elif kind == "fence-callbacks-begin":
            callbacks[host] = fences.setdefault(a, {"id": hex(a), "phases_ms": {}})
            callbacks[host]["callbacks_ms"] = t
        elif kind == "fence-done":
            fence = fences.pop(a, None)
            if fence:
                fence["done_ms"] = t
                completed.append(fence)
            callbacks.pop(host, None)
        elif kind == "nv-event-armed":
            armed[a] = {"object": hex(a), "armed_ms": t,
                        "syncpoint": b & 0xffffffff, "target": b >> 32}
        elif kind == "nv-event-signal":
            signals[a] = {"object": hex(a), "signal_ms": t, "signal_host": host,
                          "syncpoint": b & 0xffffffff, "target": b >> 32}
            if host in callbacks:
                signals[a]["fence"] = callbacks[host]
        elif kind == "nv-event-signal-long" and b in signals:
            signals[b]["signal_elapsed_ms"] = a/1000
        elif kind == "sync-wait-begin":
            pending[b] = {"guest": b, "begin_ms": t, "object_count": a, "objects": []}
        elif kind == "sync-wait-object" and b >> 32 in pending:
            pending[b >> 32]["objects"].append({"index": b & 0xffffffff, "object": hex(a),
                                               **armed.get(a, {})})
        elif kind == "sync-wake-object" and b in pending:
            wait = pending[b]
            wait.update(wake_ms=t, wake_object=hex(a), wake_host=host)
            selected = next((o for o in wait["objects"] if o["object"] == hex(a)), None)
            signal = signals.get(a)
            if selected and signal and signal["signal_ms"] <= t and all(
                    selected.get(k) == signal[k] for k in ("syncpoint", "target")):
                wait["nv_signal"] = signal
        elif kind == "sync-wait-end" and b in pending:
            wait = pending.pop(b)
            wait.update(end_ms=t, result=hex(a), elapsed_ms=round(t-wait["begin_ms"], 3))
            if "wake_ms" in wait:
                wait["wake_to_resume_ms"] = round(t-wait["wake_ms"], 3)
            waits.append(wait)
    return waits, completed, len(pending)


def pipeline_chains(events):
    """Lifetime-local build identity; DXIL/translation/sign/PSO spans are nested elapsed."""
    current, rows, frontend = {}, [], []
    phase_kinds = {
        "pipeline-worker-long", "pipeline-dxil-long", "pipeline-translate-long",
        "pipeline-validator-lock-long", "pipeline-sign-long", "pipeline-pso-long",
        "pipeline-wait-long",
    }
    outcomes = {0: "compiled", 1: "hit", 2: "shared_in_flight", 3: "bypassed"}
    def row(identity):
        if identity not in current:
            value = {"id": hex(identity), "phases": []}
            current[identity] = value
            rows.append(value)
        return current[identity]
    for t, host, kind, a, b in events:
        if kind == "pipeline-build-requested":
            value = {"id": hex(a), "key_hash": hex(b), "requested_ms": t, "phases": []}
            current[a] = value
            rows.append(value)
        elif kind == "pipeline-worker-begin":
            row(a).update(worker_begin_ms=t, worker_host=host)
        elif kind == "pipeline-build-done":
            row(a).update(done_ms=t, success=bool(b))
        elif kind == "pipeline-cache-result":
            row(b)["cache_outcome"] = outcomes.get(a, "unknown")
        elif kind in phase_kinds:
            row(b)["phases"].append({"phase": kind, "host": host,
                "start_ms": t-a/1000, "end_ms": t, "elapsed_ms": a/1000})
        elif kind == "pipeline-frontend-long":
            frontend.append({"key_hash": hex(b), "host": host,
                "start_ms": t-a/1000, "end_ms": t, "elapsed_ms": a/1000})
    for value in rows:
        if "requested_ms" in value and "worker_begin_ms" in value:
            value["queue_to_worker_ms"] = round(value["worker_begin_ms"]-value["requested_ms"], 3)
        if "requested_ms" in value and "done_ms" in value:
            value["request_to_done_ms"] = round(value["done_ms"]-value["requested_ms"], 3)
        value["phase_union_ms"] = {
            kind: covered_ms([(p["start_ms"], p["end_ms"]) for p in value["phases"]
                              if p["phase"] == kind], float("-inf"), float("inf"))
            for kind in phase_kinds if any(p["phase"] == kind for p in value["phases"])
        }
        value["frontend"] = [p for p in frontend if p["key_hash"] == value.get("key_hash") and
            p["start_ms"]-.002 <= value.get("requested_ms", float("inf")) <= p["end_ms"]+.002]
    return rows, frontend


def analyze(events, expected_vsyncs=480):
    # Log timestamps are rounded to 1us: preserve reservation order on ties.
    events = sorted(events, key=lambda e: e[0])
    counts = collections.Counter(e[2] for e in events)
    arbiter_waits, arbiter_unfinished = address_waits(events)
    sync_waits, fence_chains, sync_unfinished = completion_chains(events)
    pipelines, pipeline_frontend = pipeline_chains(events)
    pipeline_waits = [(p["start_ms"], p["end_ms"]) for r in pipelines for p in r["phases"]
                      if p["phase"] == "pipeline-wait-long"]
    queues = [e[0] for e in events if e[2] == "game-queue-buffer"]
    cpu = collections.defaultdict(list)
    uploads, gc, idle = [], [], []
    pending_ready, ready_dispatch = {}, []
    evicted, recreated = set(), 0
    run_details, run_pending = [], {}
    svc_pending, svc_waits = {}, []
    headroom, pressure_levels = [], collections.Counter()
    upload_meta, upload_phases, upload_details = {}, [], []
    ipc_pending, ipc_dispatches, ipc_phases = {}, [], []
    lease_pending, leases, lease_holds = {}, [], []
    vsync_ticks, tick_late, present_spans = [], [], []
    display_phases = []
    queue_intervals = collections.Counter()
    for t, host, event, a, b in events:
        if event == "game-queue-buffer":
            queue_intervals[f"{b & 0xffffffff}->{b >> 32}"] += 1
        if event == "vsync-tick":
            vsync_ticks.append(t)
            tick_late.append(a / 1000)
        elif event == "frame-lease-acquire":
            lease_pending[b >> 32, a] = {"acquired_ms": t, "consumer": b >> 32,
                "frame": a, "interval": b & 0xffffffff}
        elif event == "frame-lease-hold":
            lease_holds.append({"at_ms": t, "consumer": b >> 32, "frame": a,
                                "remaining_vsyncs": b & 0xffffffff})
        elif event == "frame-lease-release" and (b >> 32, a) in lease_pending:
            lease = lease_pending.pop((b >> 32, a))
            lease.update({"release_decision_ms": t, "residency_ms": round(t - lease["acquired_ms"], 3),
                          "held_vsyncs": b & 0xffffffff})
            leases.append(lease)
        elif event == "host-present-long":
            present_spans.append((t - a / 1000, t))
        elif event in ("display-compose-lock", "display-compose-long"):
            display_phases.append({"phase": event, "end_ms": t,
                                   "elapsed_ms": a / 1000, "context": b})
        elif event == "guest-run-long":
            cpu[b & 255].append((t - a / 1000, t))
            detail = {"end_ms": t, "elapsed_ms": a / 1000, "guest": b >> 8, "core": b & 255,
                      "compile_ms": None, "flush_check_ms": None}
            run_details.append(detail)
            run_pending[host, b] = detail
        elif event in ("guest-run-compile", "guest-run-flush"):
            if (host, b) in run_pending:
                run_pending[host, b]["compile_ms" if event == "guest-run-compile" else "flush_check_ms"] = a / 1000
        elif event.startswith("guest-run-") and (host, b) in run_pending:
            detail = run_pending[host, b]
            if event == "guest-run-pc": detail["end_pc"] = hex(a)
            elif event == "guest-run-stop":
                detail["halt_mask"] = hex(a & 0xffffffff)
                detail["svc"] = a >> 32 if a >> 32 != 0xffffffff else None
            elif event == "guest-run-clock": detail["clock_callback_ms"] = a / 1000
            elif event == "guest-run-icache": detail["icache_callback_ms"] = a / 1000
            elif event == "guest-run-memory":
                # The final memory event also closes the Run detail: omitted timings round to0us.
                for field in ("compile_ms", "flush_check_ms", "clock_callback_ms", "icache_callback_ms"):
                    if detail.get(field) is None:
                        detail[field] = 0.0
                detail["slow_reads"] = a & 0xffffffff
                detail["slow_writes"] = a >> 32
        elif event == "texture-gc-budget":
            pressure_levels[b] += 1
            if a != (1 << 64) - 1:
                headroom.append(a / (1024 * 1024))
        elif event == "texture-upload-info":
            upload_meta[b] = {"guest_bytes": a}
        elif event == "texture-upload-format":
            upload_meta.setdefault(b, {})["format_enum"] = a
        elif event in ("texture-upload-staging", "texture-upload-read", "texture-upload-unswizzle",
                       "texture-upload-convert", "texture-upload-backend", "texture-upload-repack"):
            upload_phases.append({"phase": event.removeprefix("texture-upload-"),
                "start_ms": t - a / 1000, "end_ms": t, "elapsed_ms": a / 1000,
                "host": host, "address": hex(b), **upload_meta.get(b, {})})
        elif event == "texture-upload-long":
            uploads.append((t - a / 1000, t))
            upload_details.append({"start_ms": t - a / 1000, "end_ms": t,
                "elapsed_ms": a / 1000, "host": host, "address": hex(b), **upload_meta.get(b, {})})
        elif event == "guest-ipc-command":
            detail = {"at_ms": t, "guest": b, "command": a & 0xffffffff,
                      "type": (a >> 32) & 0xffff, "name_length": a >> 48,
                      "service_prefix": "", "name_truncated": a >> 48 > 24}
            ipc_pending[b] = detail
            ipc_dispatches.append(detail)
        elif event in ("guest-ipc-name0", "guest-ipc-name1", "guest-ipc-name2") and b in ipc_pending:
            ipc_pending[b]["service_prefix"] += a.to_bytes(8, "little").split(b"\0", 1)[0].decode("utf-8", "replace")
        elif event in ("guest-ipc-lock", "guest-ipc-handler"):
            ipc_phases.append({"phase": event.removeprefix("guest-ipc-"),
                "start_ms": t - a / 1000, "end_ms": t, "elapsed_ms": a / 1000,
                "guest": b, **{k: v for k, v in ipc_pending.get(b, {}).items()
                              if k in ("command", "type", "service_prefix", "name_truncated")}})
        elif event == "texture-gc-long":
            gc.append((t - a / 1000, t))
        elif event == "gpu-thread-got-work":
            idle.append((t - a / 1000, t))
        elif event == "guest-thread-ready":
            pending_ready[a] = t
        elif event == "guest-svc-begin":
            pending_ready.pop(a, None)
            svc_pending[a, b] = t
        elif event == "guest-svc-end" and (a, b) in svc_pending:
            svc_waits.append({"guest": a, "svc": b, "start_ms": svc_pending.pop((a, b)), "end_ms": t})
        elif event == "guest-svc-long":
            svc_waits.append({"guest": b >> 8, "svc": b & 255,
                              "start_ms": t - a / 1000, "end_ms": t})
        elif event == "guest-dispatch" and a in pending_ready:
            ready_dispatch.append(t - pending_ready.pop(a))
        elif event == "texture-evict":
            evicted.add(a)
        elif event == "texture-create" and a in evicted:
            recreated += 1
            evicted.remove(a)
    location_history = collections.defaultdict(list)
    for t, host, kind, a, b in events:
        if kind == "guest-svc-live-pc":
            location_history[b >> 8, b & 255].append((t, hex(a)))
    for wait in svc_waits:
        history = location_history[wait["guest"], wait["svc"]]
        index = bisect.bisect_left(history, (wait["start_ms"] - .002, ""))
        if index < len(history) and history[index][0] <= wait["end_ms"]:
            wait["pc"] = history[index][1]
    # A caller can have multiple service dispatches (deferred retries) before its SVC ends.
    dispatch_by_guest = collections.defaultdict(list)
    for d in ipc_dispatches:
        dispatch_by_guest[d["guest"]].append(d)
    dispatch_times = {g: [d["at_ms"] for d in ds] for g, ds in dispatch_by_guest.items()}
    for w in svc_waits:
        if w["svc"] not in (33, 34):
            continue
        ds = dispatch_by_guest[w["guest"]]
        begin = bisect.bisect_left(dispatch_times.get(w["guest"], []), w["start_ms"])
        end = bisect.bisect_right(dispatch_times.get(w["guest"], []), w["end_ms"])
        w["dispatch_count"] = end - begin
        w["dispatches"] = ds[begin:min(end, begin + 3)]
    ipc_groups = collections.defaultdict(list)
    for w in svc_waits:
        if w.get("dispatch_count") == 1:
            d = w["dispatches"][0]
            ipc_groups[w["guest"], d["service_prefix"], d["command"], d["type"]].append(w["end_ms"] - w["start_ms"])
    for u in upload_details:
        u["phases"] = [p for p in upload_phases if p["host"] == u["host"] and
                       p["address"] == u["address"] and
                       u["start_ms"] - .002 <= p["start_ms"] and p["end_ms"] <= u["end_ms"] + .002]
    for wait in arbiter_waits:
        source = wait.get("signalling_guest")
        if source is None:
            continue
        start, end = wait["begin_ms"], wait["wake_ms"]
        runs = [r for r in run_details if r["guest"] == source and
                r["end_ms"] - r["elapsed_ms"] < end and r["end_ms"] > start]
        waits = [w for w in svc_waits if w["guest"] == source and
                 w["start_ms"] < end and w["end_ms"] > start]
        wait["signaller_long_run_overlap_ms"] = covered_ms(
            [(r["end_ms"] - r["elapsed_ms"], r["end_ms"]) for r in runs], start, end)
        wait["signaller_svc_overlap_ms"] = covered_ms(
            [(w["start_ms"], w["end_ms"]) for w in waits], start, end)
        wait["signaller_runs"] = sorted(runs, key=lambda r: r["elapsed_ms"], reverse=True)[:4]
        wait["signaller_waits"] = sorted(waits,
            key=lambda w: w["end_ms"] - w["start_ms"], reverse=True)[:4]
    gaps = [(b - a, a, b) for a, b in zip(queues, queues[1:])]
    longest = []
    for duration, start, end in sorted(gaps, reverse=True)[:8]:
        inside = collections.Counter(e[2] for e in events if start <= e[0] <= end)
        longest.append({
            "start_ms": start, "end_ms": end, "gap_ms": round(duration, 3),
            "long_guest_run_overlap_ms_by_core": {k: covered_ms(v, start, end) for k, v in cpu.items()},
            "long_upload_overlap_ms": covered_ms(uploads, start, end),
            "long_gc_overlap_ms": covered_ms(gc, start, end),
            "pipeline_wait_overlap_ms": covered_ms(pipeline_waits, start, end),
            "pipeline_frontend_overlap_ms": covered_ms(
                [(p["start_ms"], p["end_ms"]) for p in pipeline_frontend], start, end),
            "gpu_idle_overlap_ms": covered_ms(idle, start, end),
            "host_present_overlap_ms": covered_ms(present_spans, start, end),
            "queue_intervals_inside": dict(collections.Counter(
                f"{e[4] & 0xffffffff}->{e[4] >> 32}" for e in events
                if e[2] == "game-queue-buffer" and start <= e[0] <= end)),
            "held_leases_overlapping": [lease for lease in leases
                if lease["acquired_ms"] < end and lease["release_decision_ms"] > start],
            "creates": inside["texture-create"], "gc_evictions": inside["texture-evict"],
            "dispatches": inside["guest-dispatch"],
            "svc_waits_overlapping": sorted([
                {"guest": w["guest"], "svc": w["svc"],
                 "overlap_ms": round(min(end, w["end_ms"]) - max(start, w["start_ms"]), 3),
                 "dispatches": w.get("dispatches", [])}
                for w in svc_waits if w["start_ms"] < end and w["end_ms"] > start
            ], key=lambda w: w["overlap_ms"], reverse=True)[:8],
        })
    return {
        "events": len(events), "vsyncs": counts["vsync"], "queued": len(queues),
        "expected_vsyncs": expected_vsyncs,
        "complete": counts["vsync"] == expected_vsyncs,
        "fps_estimate": len(queues) * 60 / expected_vsyncs
                        if counts["vsync"] == expected_vsyncs else None,
        "gap_p99_ms": percentile([g[0] for g in gaps], .99),
        "gaps_ge_25ms": sum(g[0] >= 25 for g in gaps),
        "pipeline_cache_outcomes": dict(collections.Counter(
            r["cache_outcome"] for r in pipelines if "cache_outcome" in r)),
        "pipeline_builds_completed": sum("done_ms" in r for r in pipelines),
        "pipeline_chains_longest": sorted(pipelines,
            key=lambda r: max(r.get("request_to_done_ms", 0),
                              r["phase_union_ms"].get("pipeline-wait-long", 0)), reverse=True)[:12],
        "pipeline_frontend_longest": sorted(pipeline_frontend,
            key=lambda p: p["elapsed_ms"], reverse=True)[:8],
        "sync_wait_count": len(sync_waits), "sync_wait_unfinished": sync_unfinished,
        "sync_wait_longest": sorted(sync_waits, key=lambda w: w["elapsed_ms"], reverse=True)[:16],
        "sync_wait_nv_linked": sum("nv_signal" in w for w in sync_waits),
        "fence_chains_completed": len(fence_chains),
        "fence_chains_longest": sorted(fence_chains,
            key=lambda f: f.get("done_ms", 0)-f.get("queued_ms", f.get("done_ms", 0)), reverse=True)[:12],
        "address_wait_count": len(arbiter_waits),
        "address_wait_unfinished": arbiter_unfinished,
        "address_wait_longest": sorted(arbiter_waits, key=lambda w: w["elapsed_ms"], reverse=True)[:16],
        "address_signalling_guests": dict(collections.Counter(
            w.get("signalling_guest", "cancel_or_unknown") for w in arbiter_waits)),
        "counts": dict(counts), "recreates_after_gc_same_address": recreated,
        "actual_app_headroom_min_mib": min(headroom) if headroom else None,
        "gc_pressure_levels": dict(pressure_levels),
        "gc_readback_states": dict(collections.Counter(
            e[3] for e in events if e[2] == "texture-gc-readback")),
        "texture_heap_samples_mib": [
            {"at_ms": e[0], "heap": e[3] / 1048576, "reserved": e[4] / 1048576}
            for e in events if e[2] == "texture-heap-usage"][::60],
        "gc_pinned_peak_mib": max(
            (e[4] / 1048576 for e in events if e[2] == "texture-heap-pending"), default=None),
        "texture_fence_pending_peak_mib": max(
            (e[3] / 1048576 for e in events if e[2] == "texture-heap-pending"), default=None),
        "texture_heap_free_samples_mib": [
            {"at_ms": e[0], "free": e[3] / 1048576, "largest": e[4] / 1048576}
            for e in events if e[2] == "texture-heap-free"][::60],
        "queue_requested_applied_intervals": dict(queue_intervals),
        "vsync_tick_count": len(vsync_ticks),
        "vsync_tick_lateness_p99_ms": percentile(tick_late, .99),
        "vsync_tick_lateness_max_ms": max(tick_late) if tick_late else None,
        "vsync_tick_gap_max_ms": max((b - a for a, b in zip(vsync_ticks, vsync_ticks[1:])), default=None),
        "leases_completed": len(leases), "leases_unfinished": len(lease_pending),
        "lease_intervals": dict(collections.Counter(lease["interval"] for lease in leases)),
        "leases_held_extra_vsyncs": sum(lease["held_vsyncs"] > lease["interval"] for lease in leases),
        "longest_leases": sorted(leases, key=lambda l: l["residency_ms"], reverse=True)[:12],
        "longest_display_phases": sorted(display_phases, key=lambda p: p["elapsed_ms"], reverse=True)[:12],
        "host_present_spans": len(present_spans),
        "host_present_max_ms": max((b - a for a, b in present_spans), default=None),
        "longest_guest_runs": sorted(run_details, key=lambda r: r["elapsed_ms"], reverse=True)[:8],
        "ipc_wait_by_guest_command": [
            {"guest": g, "service_prefix": name, "command": cmd, "type": typ,
             "requests": len(ds), "elapsed_sum_ms": round(sum(ds), 3),
             "max_ms": round(max(ds), 3)}
            for (g, name, cmd, typ), ds in sorted(ipc_groups.items())],
        "longest_uploads": sorted(upload_details, key=lambda u: u["elapsed_ms"], reverse=True)[:8],
        "longest_upload_phases": sorted(upload_phases, key=lambda p: p["elapsed_ms"], reverse=True)[:12],
        "ipc_dispatch_count": len(ipc_dispatches),
        "ipc_dispatch_names_truncated": sum(d["name_truncated"] for d in ipc_dispatches),
        "longest_ipc_phases": sorted(ipc_phases, key=lambda p: p["elapsed_ms"], reverse=True)[:12],
        "longest_ipc_waits": sorted([w for w in svc_waits if w["svc"] in (33, 34)],
                                   key=lambda w: w["end_ms"] - w["start_ms"], reverse=True)[:12],
        "host_cpu_windows": [
            {"at_ms": t, "host": host, "core": b >> 56,
             "cpu_ms": a / 1000 if a != (1 << 64) - 1 else None,
             "wall_ms": (b & ((1 << 56) - 1)) / 1000}
            for t, host, event, a, b in events if event == "guest-host-cpu-window"],
        "svc_waits_paired": len(svc_waits) - counts["guest-svc-long"],
        "svc_waits_completed": len(svc_waits), "svc_waits_unfinished": len(svc_pending),
        "longest_svc_waits": sorted(svc_waits,
            key=lambda w: w["end_ms"] - w["start_ms"], reverse=True)[:8],
        "ready_to_dispatch_p99_ms": percentile(ready_dispatch, .99),
        "ready_to_dispatch_samples": len(ready_dispatch), "longest_gaps": longest,
        "limits": "Spans >=200us completed within T only; overlap is not causation. "
                  "Run includes callbacks/compilation/preemption, not host CPU utilization. "
                  "Pipeline worker/DXIL/translation/sign/PSO/wait are nested or concurrent elapsed, never sum phases. "
                  "Builds begun before T have unknown queue delay; absent short phases are unknown. "
                  "Compile and flush are nested elapsed and can overlap; never sum or label the remainder CPU busy. "
                  "Host CPU is OS-accounted user+kernel in >=100ms windows, may be coarse; absent is unknown. "
                  "End PC is not a hot-PC sample. SVC spans pair by guest across host migration; unpaired borders omitted. "
                  "Lease release events record decisions, not GPU completion; policy counts normalize overlay interval1. "
                  "Vsync tick lateness is CoreTiming-reported; lease elapsed includes host timing, not a missed-vsync verdict. "
                  "DXGI Present elapsed is host blocking, not GPU duration. "
                  "Upload phases >=200us only; repack is nested in backend, never sum them. "
                  "IPC dispatch identifies service prefix up to24bytes and command/type; long names flagged. "
                  "Handler elapsed can include deferred response setup; it is not request completion. "
                  "Dispatches before capture are unknown; multiple dispatches can be retries; summaries include only single dispatch. "
                  "Sleep/lock spans shorter than 200us or crossing capture borders are omitted. "
                  "A sleeping guest overlapping a gap does not establish it causes that gap. "
                  "Upload is host elapsed, not GPU time. Same-address recreation is a heuristic. "
                  "Ready-to-dispatch samples exclude switches without a captured Runnable transition.",
    }


def read(path):
    captures = []
    with path.open(encoding="utf-8", errors="replace") as lines:
        for line in lines:
            if re.search(r"Frame trace: \d+ events", line):
                captures.append([])
            match = re.search(r"Dump: FT\s+([\d.]+)\s+(\d+)\s+(\S+)\s+(\d+)\s+(\d+)", line)
            if match and captures:
                t, host, event, a, b = match.groups()
                captures[-1].append((float(t), int(host), event, int(a), int(b)))
    return captures


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--vsyncs", type=int, default=480, choices=(240, 480),
                        help="expected capture window: 480 (8s), or 240 for older 4s logs")
    args = parser.parse_args()
    for number, events in enumerate(read(args.log), 1):
        print(json.dumps({"capture": number, **analyze(events, args.vsyncs)}, ensure_ascii=False))

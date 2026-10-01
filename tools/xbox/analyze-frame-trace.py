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


def analyze(events, expected_vsyncs=480):
    # Log timestamps are rounded to 1us: preserve reservation order on ties.
    events = sorted(events, key=lambda e: e[0])
    counts = collections.Counter(e[2] for e in events)
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
    for t, host, event, a, b in events:
        if event == "guest-run-long":
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
    gaps = [(b - a, a, b) for a, b in zip(queues, queues[1:])]
    longest = []
    for duration, start, end in sorted(gaps, reverse=True)[:8]:
        inside = collections.Counter(e[2] for e in events if start <= e[0] <= end)
        longest.append({
            "start_ms": start, "end_ms": end, "gap_ms": round(duration, 3),
            "long_guest_run_overlap_ms_by_core": {k: covered_ms(v, start, end) for k, v in cpu.items()},
            "long_upload_overlap_ms": covered_ms(uploads, start, end),
            "long_gc_overlap_ms": covered_ms(gc, start, end),
            "gpu_idle_overlap_ms": covered_ms(idle, start, end),
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
                  "Compile and flush are nested elapsed and can overlap; never sum or label the remainder CPU busy. "
                  "Host CPU is OS-accounted user+kernel in >=100ms windows, may be coarse; absent is unknown. "
                  "End PC is not a hot-PC sample. SVC spans pair by guest across host migration; unpaired borders omitted. "
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

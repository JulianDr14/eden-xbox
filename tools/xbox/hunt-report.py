# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bottleneck report from a hunt capture ("Capture performance" in the game menu of a package
built with package-appx.ps1 -Hunt, or F8 with developer hotkeys).

    python tools/xbox/hunt-report.py jadyx_hunt.txt [--capture N] [--top 15] [--out report.md]
        [--profile jadyx_hunt_profile.txt] [--exe build-uwp/bin/jadyx.exe] [--no-profile]

Both files are in LocalState/jadyx/log on the console. The report says who limits the frame
(the GPU, the GPU thread recording draws, or the guest CPU), then where the draws come from
and what they cost: per kind and skip reason, per render target (pass), per pipeline, per guest
macro, runs of draws that could be batched, and repeated draw signatures. With the CPU profile
next to the capture (or --profile), profile-report.py's stacks are appended.

GPU times are timestamp deltas: each call's time runs from the previous call of the same command
list, so barriers, copies and uploads recorded in between count for the call after them. The first
call of a command list after a flush has no time ("unknown").
"""
import argparse
import collections
import os
import subprocess
import sys

KINDS = ["draw", "draw indexed", "draw indirect", "draw texture", "clear", "dispatch"]
OUTCOMES = ["recorded", "skipped", "conditional", "culled", "no pipeline", "PSO rejected",
            "skipped shader", "empty", "no target", "expanded on CPU"]
RECORDED, EXPANDED = 0, 9
CPU_INDIRECT, EMULATED_TOPOLOGY = 1, 2
TOPOLOGIES = ["points", "lines", "line loop", "line strip", "triangles", "triangle strip",
              "triangle fan", "quads", "quad strip", "polygon", "lines adj", "line strip adj",
              "triangles adj", "triangle strip adj", "patches"]
DXGI = {0: "-", 2: "RGBA32F", 10: "RGBA16F", 11: "RGBA16", 16: "RG32F", 20: "D32S8",
        24: "RGB10A2", 26: "R11G11B10F", 28: "RGBA8", 29: "RGBA8_SRGB", 34: "RG16F", 35: "RG16",
        40: "D32F", 41: "R32F", 42: "R32U", 45: "D24S8", 49: "RG8", 54: "R16F", 55: "D16",
        56: "R16", 61: "R8", 87: "BGRA8", 91: "BGRA8_SRGB"}


class Record:
    __slots__ = ("kind", "outcome", "flags", "frame", "pass_", "vs", "ps", "count", "instances",
                 "topology", "cpu_ns", "pipeline_ns", "gpu_ns", "macro")

    def __init__(self, fields):
        self.kind, self.outcome, self.flags, self.frame = (int(x) for x in fields[:4])
        self.pass_ = int(fields[4])
        self.vs, self.ps = fields[5], fields[6]
        self.count, self.instances, self.topology = int(fields[7]), int(fields[8]), int(fields[9])
        self.cpu_ns, self.pipeline_ns = int(fields[10]), int(fields[11])
        self.gpu_ns = None if fields[12] == "-" else int(fields[12])
        self.macro = int(fields[13], 16)


class Capture:
    def __init__(self, number):
        self.number = number
        self.frames = []
        self.passes = {}
        self.perf = {}
        self.meta = {}
        self.records = []
        self.dropped = 0


def parse(path):
    captures = []
    current = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            tag = p[0]
            if tag == "hunt":
                current = Capture(int(p[3]))
                current.dropped = int(p[9])
                captures.append(current)
            elif current is None:
                continue
            elif tag == "r":
                current.records.append(Record(p[1:]))
            elif tag == "frame":
                current.frames.append(float(p[2]))
            elif tag == "pass":
                current.passes[int(p[1])] = describe_pass(p[2:])
            elif tag == "perf":
                current.perf[p[1]] = int(p[2])
            elif tag == "meta":
                it = iter(p[1:])
                current.meta = {k: float(v) for k, v in zip(it, it)}
    return captures


def describe_pass(p):
    size, samples, colors, color, depth = p
    text = f"{size} {DXGI.get(int(color[1:]), 'fmt' + color[1:])}"
    if int(colors[1:]) > 1:
        text += f" x{colors[1:]} RT"
    if int(depth[1:]):
        text += f" + {DXGI.get(int(depth[1:]), 'fmt' + depth[1:])}"
    if int(samples[1:]) > 1:
        text += f" MSAA{samples[1:]}"
    return text


def ms(ns):
    return ns / 1e6


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(out)


def shader(vs, ps):
    if int(vs, 16) == 0:
        return f"CS {ps}" if int(ps, 16) else "-"
    return f"VS {vs} / PS {ps}"


def verdict(c, frames, avg):
    perf, counters_ms = c.perf, c.meta.get("counters_ms", 0.0)
    if counters_ms <= 0:
        return ["No counter time: old capture?"], {}

    def share(name, scale):
        return perf.get(name, 0) / scale / counters_ms

    shares = {
        "gpu": share("GpuBusyUs", 1e3),
        "idle": share("GpuThreadIdleUs", 1e3),
        "fence": share("FenceWaitUs", 1e3),
        "flush": share("GpuThreadFlushUs", 1e3),
        "pipeline": share("PipelineStallUs", 1e3),
        "guest_wait": share("GuestGpuWaitUs", 1e3),
    }
    recorded = [r for r in c.records if r.outcome != EXPANDED]
    calls_ms = ms(sum(r.cpu_ns for r in recorded)) / frames
    lines = [f"- Frame: **{avg:.2f} ms** average ({1000 / avg:.1f} FPS) over {frames} frames; "
             f"worst {max(c.frames):.2f} ms."]
    lines.append(f"- GPU busy: **{shares['gpu'] * 100:.0f}%** of the time "
                 f"({shares['gpu'] * avg:.2f} ms per frame).")
    lines.append(f"- GPU thread: {calls_ms:.2f} ms per frame inside draws/clears/dispatches, "
                 f"{shares['idle'] * 100:.0f}% idle waiting for the guest, "
                 f"{shares['fence'] * 100:.0f}% waiting on the GPU fence, "
                 f"{shares['flush'] * 100:.0f}% in guest readback flushes, "
                 f"{shares['pipeline'] * 100:.0f}% waiting for pipelines.")
    if shares["gpu"] >= 0.85:
        cause = "**GPU-bound**: the GPU is busy almost all the time. Cut GPU work (passes and " \
                "pipelines with most GPU ms below), not draw count alone."
    elif shares["idle"] >= 0.30:
        cause = "**Guest CPU-bound**: the GPU thread waits for the game's command lists. The " \
                "draws below are not the limit; look at the CPU profile (JIT, HLE services)."
    elif shares["fence"] + shares["flush"] >= 0.25:
        cause = "**Synchronization-bound**: the GPU thread blocks on the GPU (fence waits or " \
                "guest readbacks). Look for flushes and downloads, not draw cost."
    else:
        cause = "**GPU-thread CPU-bound**: recording calls fills the frame while the GPU has room. " \
                "Fewer or cheaper draws pay off directly: see the counts and CPU columns below."
    lines.append(f"- Verdict: {cause}")
    return lines, shares


def report(c, top, out):
    frames = max(1, len(c.frames))
    avg = sum(c.frames) / frames if c.frames else 0.0
    out.append(f"# Bottleneck hunt — capture {c.number}\n")
    if not c.records:
        out.append("No records in this capture.")
        return
    lines, _ = verdict(c, frames, avg)
    out.append("## Verdict\n")
    out += lines
    known = [r for r in c.records if r.outcome == RECORDED]
    timed = [r for r in known if r.gpu_ns is not None]
    gpu_total = sum(r.gpu_ns for r in timed)
    out.append(f"- GPU time attributed to calls: {ms(gpu_total) / frames:.2f} ms per frame "
               f"({len(timed)} of {len(known)} recorded calls timed).")
    if c.dropped:
        out.append(f"- **{c.dropped} records dropped** (capacity): the tail of the capture is "
                   "missing.")

    # Draw accounting.
    out.append("\n## Calls per frame\n")
    by_kind = collections.Counter()
    for r in c.records:
        by_kind[(r.kind, r.outcome)] += 1
    rows = []
    for kind in range(len(KINDS)):
        recorded = by_kind[(kind, RECORDED)]
        skipped = {OUTCOMES[o]: n for (k, o), n in by_kind.items()
                   if k == kind and o not in (RECORDED, EXPANDED)}
        expanded = by_kind[(kind, EXPANDED)]
        if not recorded and not skipped and not expanded:
            continue
        cpu = sum(r.cpu_ns for r in c.records if r.kind == kind and r.outcome != EXPANDED)
        gpu = sum(r.gpu_ns or 0 for r in known if r.kind == kind)
        skip_text = ", ".join(f"{k} {v / frames:.1f}" for k, v in
                              sorted(skipped.items(), key=lambda kv: -kv[1])) or "-"
        if expanded:
            skip_text += f"; {expanded / frames:.1f} expanded on CPU"
        rows.append([KINDS[kind], f"{recorded / frames:.1f}", skip_text,
                     f"{ms(cpu) / frames:.2f}", f"{ms(gpu) / frames:.2f}",
                     f"{cpu / max(1, recorded) / 1e3:.1f}"])
    out.append(table(["kind", "recorded", "skipped (per frame)", "CPU ms", "GPU ms",
                      "CPU µs/call"], rows))
    cpu_indirect = [r for r in c.records if r.flags & CPU_INDIRECT]
    emulated = [r for r in known if r.flags & EMULATED_TOPOLOGY]
    expanded = [r for r in c.records if r.outcome == EXPANDED]
    if expanded:
        out.append(f"\n- Indirect calls expanded on the CPU: {len(expanded) / frames:.1f} per "
                   f"frame became {len(cpu_indirect) / frames:.1f} draws "
                   f"({ms(sum(r.cpu_ns for r in expanded)) / frames:.2f} ms CPU per frame, "
                   "guest-memory reads included).")
    if emulated:
        out.append(f"- Emulated topologies (quads/loops/fans rebuilt on the CPU): "
                   f"{len(emulated) / frames:.1f} draws per frame, "
                   f"{ms(sum(r.cpu_ns for r in emulated)) / frames:.2f} ms CPU.")
    pipeline_wait = sum(r.pipeline_ns for r in c.records)
    out.append(f"- Pipeline lookup/wait inside calls: {ms(pipeline_wait) / frames:.2f} ms per frame.")
    tiny = [r for r in known if r.kind in (0, 1) and r.count * max(1, r.instances) <= 6]
    if tiny:
        out.append(f"- Tiny draws (≤ 6 vertices in total): {len(tiny) / frames:.1f} per frame, "
                   f"{ms(sum(r.cpu_ns for r in tiny)) / frames:.2f} ms CPU.")

    # GPU-thread stages from the counters.
    stages = [("DrawTexturesNs", "texture bindings"), ("DrawBuffersNs", "buffers"),
              ("DrawDescriptorsNs", "descriptors"), ("DrawTargetsNs", "render targets"),
              ("DrawSamplersNs", "samplers"), ("DrawRecordNs", "state + record"),
              ("ClearNs", "clears"), ("DispatchNs", "dispatches"),
              ("TextureDecodeUs", "texture decode"), ("ResourceCreateUs", "resource creation"),
              ("CacheInvalidationNs", "cache invalidation")]
    counters_ms = c.meta.get("counters_ms", 0.0)
    if counters_ms > 0 and avg > 0:
        rows = []
        for name, label in stages:
            value = c.perf.get(name, 0)
            value_ms = value / (1e3 if name.endswith("Us") else 1e6)
            per_frame = value_ms / counters_ms * avg
            if per_frame >= 0.01:
                rows.append([label, f"{per_frame:.2f}"])
        if rows:
            out.append("\n## Where the GPU thread's draw time goes (ms per frame)\n")
            out.append(table(["stage", "ms"], sorted(rows, key=lambda r: -float(r[1]))))
        per_frame = lambda name: c.perf.get(name, 0) / counters_ms * avg
        out.append(f"\nSubmits {per_frame('Submits'):.1f}/frame, PSO creations "
                   f"{per_frame('PsoCreates'):.1f}, texture uploads "
                   f"{per_frame('TextureUploads'):.1f} "
                   f"({per_frame('TextureUploadBytes') / 1048576:.2f} MiB), downloads "
                   f"{per_frame('TextureDownloads'):.1f}, guest readback flushes "
                   f"{per_frame('GpuThreadFlushes'):.1f}, resources created "
                   f"{per_frame('ResourcesCreated'):.1f}.")

    # Passes.
    group(out, "Render targets (passes)", known, frames, top,
          key=lambda r: r.pass_, label=lambda k: c.passes.get(k, "dispatch / no target"),
          extra=pass_entries(c.records))
    group(out, "Pipelines", known, frames, top, key=lambda r: (r.vs, r.ps),
          label=lambda k: shader(*k))
    macro_records = [r for r in c.records if r.outcome != EXPANDED]
    group(out, "Guest macros that issue the calls", macro_records, frames, top,
          key=lambda r: r.macro, label=lambda k: f"macro 0x{k:x}" if k else "no macro (direct)")

    # Batching.
    out.append("\n## Batching candidates\n")
    runs = collections.defaultdict(lambda: [0, 0, 0])  # draws in runs, runs, cpu ns
    previous, length, cpu = None, 0, 0

    def close():
        if previous is not None and length >= 4:
            entry = runs[previous]
            entry[0] += length
            entry[1] += 1
            entry[2] += cpu

    for r in known:
        key = (r.frame, r.pass_, r.vs, r.ps, r.kind)
        if r.kind in (0, 1) and key == previous:
            length += 1
            cpu += r.cpu_ns
            continue
        close()
        previous, length, cpu = (key if r.kind in (0, 1) else None), 1, r.cpu_ns
    close()
    merged = collections.defaultdict(lambda: [0, 0, 0])
    for (frame, pass_, vs, ps, kind), (draws, count, cpu_ns) in runs.items():
        entry = merged[(pass_, vs, ps)]
        entry[0] += draws
        entry[1] += count
        entry[2] += cpu_ns
    rows = sorted(merged.items(), key=lambda kv: -kv[1][0])[:top]
    if rows:
        out.append("Back-to-back draws with the same target and pipeline (runs of 4 or more): "
                   "instancing or merging them saves the per-draw CPU cost.\n")
        out.append(table(["pipeline", "target", "draws/frame", "runs/frame", "avg run",
                          "CPU ms/frame"],
                         [[shader(vs, ps), c.passes.get(pass_, "-"), f"{d / frames:.1f}",
                           f"{n / frames:.1f}", f"{d / max(1, n):.1f}", f"{ms(t) / frames:.2f}"]
                          for (pass_, vs, ps), (d, n, t) in rows]))
    else:
        out.append("No runs of 4 or more equal draws.")

    signatures = collections.Counter((r.frame, r.pass_, r.vs, r.ps, r.count, r.instances,
                                      r.topology) for r in known if r.kind in (0, 1))
    repeated = collections.Counter()
    for (frame, pass_, vs, ps, count, instances, topology), n in signatures.items():
        if n > 1:
            repeated[(pass_, vs, ps, count, instances, topology)] += n
    rows = repeated.most_common(top)
    if rows:
        out.append("\nRepeated draw signatures (same target, pipeline, count and instances in a "
                   "frame; the data may still differ):\n")
        out.append(table(["pipeline", "target", "vertices", "instances", "topology", "per frame"],
                         [[shader(vs, ps), c.passes.get(pass_, "-"), count, instances,
                           TOPOLOGIES[t] if t < len(TOPOLOGIES) else t, f"{n / frames:.1f}"]
                          for (pass_, vs, ps, count, instances, t), n in rows]))

    # Frames.
    out.append("\n## Frames\n")
    rows = []
    for f, interval in enumerate(c.frames):
        rs = [r for r in known if r.frame == f]
        rows.append([f, f"{interval:.2f}", len(rs), f"{ms(sum(r.cpu_ns for r in rs)):.2f}",
                     f"{ms(sum(r.gpu_ns or 0 for r in rs)):.2f}"])
    out.append(table(["frame", "interval ms", "recorded calls", "CPU ms", "GPU ms"], rows))


def pass_entries(records):
    """How many times per pass the stream switches into it (render pass changes)."""
    entries = collections.Counter()
    previous = None
    for r in records:
        if r.outcome != RECORDED or r.pass_ < 0:
            continue
        if r.pass_ != previous:
            entries[r.pass_] += 1
        previous = r.pass_
    return entries


def group(out, title, records, frames, top, key, label, extra=None):
    # calls, cpu, gpu, vertices, pipeline, draws (clears count pixels and dispatches groups, so
    # only direct draws feed the vertex average)
    stats = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0])
    for r in records:
        s = stats[key(r)]
        s[0] += 1
        s[1] += r.cpu_ns
        s[2] += r.gpu_ns or 0
        s[4] += r.pipeline_ns
        if r.kind in (0, 1):
            s[3] += r.count * max(1, r.instances)
            s[5] += 1
    if not stats:
        return
    total_gpu = sum(s[2] for s in stats.values()) or 1
    total_cpu = sum(s[1] for s in stats.values()) or 1
    header = ["", "calls/frame", "GPU ms", "GPU %", "CPU ms", "CPU %", "pipeline ms",
              "avg vertices"]
    if extra is not None:
        header.append("entered/frame")
    out.append(f"\n## {title}\n")
    for order, name in ((2, "by GPU time"), (1, "by GPU-thread CPU time"), (0, "by calls")):
        ranked = sorted(stats.items(), key=lambda kv: -kv[1][order])[:top]
        rows = []
        for k, s in ranked:
            row = [label(k), f"{s[0] / frames:.1f}", f"{ms(s[2]) / frames:.2f}",
                   f"{100 * s[2] / total_gpu:.0f}%", f"{ms(s[1]) / frames:.2f}",
                   f"{100 * s[1] / total_cpu:.0f}%", f"{ms(s[4]) / frames:.2f}",
                   f"{s[3] / s[5]:.0f}" if s[5] else "-"]
            if extra is not None:
                row.append(f"{extra.get(k, 0) / frames:.1f}")
            rows.append(row)
        out.append(f"\n### {name}\n")
        out.append(table(header, rows))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("hunt", help="jadyx_hunt.txt")
    parser.add_argument("--capture", type=int, help="capture number; the last one by default")
    parser.add_argument("--top", type=int, default=15)
    parser.add_argument("--out", help="write the report here instead of printing it")
    parser.add_argument("--profile", help="CPU profile; jadyx_hunt_profile.txt next to it by default")
    parser.add_argument("--exe", default=os.path.join("build-uwp", "bin", "jadyx.exe"))
    parser.add_argument("--no-profile", action="store_true", help="skip the CPU profile")
    args = parser.parse_args()

    captures = parse(args.hunt)
    if not captures:
        sys.exit(f"{args.hunt}: no hunt captures")
    if args.capture is None:
        capture = captures[-1]
    else:
        matches = [c for c in captures if c.number == args.capture]
        if not matches:
            sys.exit(f"capture {args.capture} not found; there are "
                     f"{', '.join(str(c.number) for c in captures)}")
        capture = matches[0]
    out = []
    report(capture, args.top, out)

    profile = args.profile or os.path.join(os.path.dirname(os.path.abspath(args.hunt)),
                                           "jadyx_hunt_profile.txt")
    if not args.no_profile and os.path.exists(profile):
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "profile-report.py")
        result = subprocess.run([sys.executable, script, profile, "--exe", args.exe,
                                 "--top", str(args.top)], capture_output=True, text=True,
                                encoding="utf-8", errors="replace")
        out.append("\n# CPU profile (every thread, during the capture)\n")
        if result.returncode == 0:
            out.append(result.stdout.replace("\n# ", "\n## "))
        else:
            out.append(f"profile-report.py failed:\n\n```\n{result.stderr.strip()}\n```")
    text = "\n".join(out) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"wrote {args.out}")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)


if __name__ == "__main__":
    main()

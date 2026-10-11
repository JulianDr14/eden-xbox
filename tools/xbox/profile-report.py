# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turns the in-process sampling profile (boot.cfg "profile=1", LocalState/eden/log/eden_profile.txt)
into a bottleneck report, with the build's PDB.

    python tools/xbox/profile-report.py eden_profile.txt [--exe build-uwp/bin/eden-uwp.exe]
        [--window N[-M]] [--top 25] [--out report.md] [--folded profile.folded]

The report starts with a verdict (which threads are saturated and what fills them), then per
thread: CPU time, the subsystems its time goes to, the hottest functions on their own and with
what they call, which code calls into the OS and the driver, and where the thread waits.
--folded writes the stacks with function names in the folded format of flame graphs; drop the file
on https://www.speedscope.app to browse them.

Sample states: R means the thread ran since the previous sample, W that it did not (it was blocked;
those are sampled less often and weighed accordingly). A thread that blocks and wakes many times
between two samples is R but caught inside its wait: an R sample whose leaf is in an OS module
under one of our waits counts as waiting here, not as CPU work.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pdb_symbols import Resolver, pe_timestamp  # noqa: E402

JIT = "[JIT guest code]"
SATURATED = 0.85  # of one core

# Subsystem of a sample: the first eden-uwp.exe frame from the leaf that matches a rule.
SUBSYSTEMS = [
    (re.compile(r"TextureCache|ImageView|\bImage::|ImageBase|ASTC|Astc|Unswizzle|Swizzle"),
     "texture cache"),
    (re.compile(r"BufferCache|StagingBuffer|TransferBuffer|UploadRing|StreamBuffer"),
     "buffer cache / uploads"),
    (re.compile(r"PipelineCache|GraphicsPipeline|ComputePipeline|ShaderCompiler|SpirvToDxil|"
                r"spirv|Shader::|LinkedShader|StageBindings|RootSignature"),
     "pipelines / shaders"),
    (re.compile(r"Descriptor|Sampler"), "descriptors / samplers"),
    (re.compile(r"QueryCache|Query|ConditionalRendering|Predication"), "queries"),
    (re.compile(r"Scheduler|CommandList|FenceManager|Submit|BarrierBatch|Barrier"),
     "command lists / barriers"),
    (re.compile(r"PresentManager|Swapchain|Present|Overlay|Composite"), "presentation"),
    (re.compile(r"Rasterizer|MaxwellToD3D12|StateTracker|FixedPipelineState|Framebuffer"),
     "draw setup (rasterizer)"),
    (re.compile(r"Maxwell3D|KeplerCompute|Fermi2D|MaxwellDMA|Engines::|DmaPusher|Puller|"
                r"MacroEngine|Macro|Tegra::|GPU::|Host1x|Syncpoint"),
     "GPU command processing"),
    (re.compile(r"Dynarmic|Xbyak|A64Jit|A32Jit|BlockOfCode|EmitX64|IR::"), "JIT compiler"),
    (re.compile(r"Core::Memory|DeviceMemory|PageTable|ArmDynarmic|Memory::"),
     "guest memory access"),
    (re.compile(r"Kernel::|KThread|KScheduler|KProcess|Svc"), "HLE kernel"),
    (re.compile(r"Service::|Nvidia::|nvdrv|nvmap"), "HLE services"),
    (re.compile(r"AudioCore|XAudio2|Opus"), "audio"),
    (re.compile(r"FileSys|Loader::|Common::FS"), "file system"),
    (re.compile(r"Common::Fiber|Common::Event|WakeFlag|SpinPolicy|CpuWait"),
     "threads / synchronisation"),
]

# Our functions that block in the OS: an R sample stopped in the OS under one of them is a wait.
WAIT_FUNCTIONS = re.compile(r"Wait|Block|Sleep|condition_variable|::Pop<|Semaphore|Yield|Park")
OS_MODULE = re.compile(r"^\[(ntdll|KERNELBASE|kernel32|MSVCP140\w*|win32u)\.dll\]$", re.I)

TEMPLATE_ARGS = re.compile(r"<[^<>]*>")


def short_name(name):
    """Drops template arguments, which make some names unreadably long."""
    previous = None
    while previous != name:
        previous, name = name, TEMPLATE_ARGS.sub("<>", name)
    return name


class Profile:
    def __init__(self):
        self.timestamp = None
        self.windows = []


class Window:
    def __init__(self, fields):
        self.number = int(fields["window"])
        self.seconds = float(fields["seconds"])
        self.hz = int(fields["hz"])
        self.meta = fields
        self.threads = collections.OrderedDict()  # name -> [cpu_ms, running, waiting, count]
        self.stacks = []  # (thread, state, count, [frames root first])


def parse(path):
    profile = Profile()
    window = None
    with open(path, encoding="utf-8", errors="replace") as file:
        for raw in file:
            line = raw.rstrip("\n")
            if line.startswith("executable timestamp"):
                profile.timestamp = int(line.split()[2], 16)
            elif line.startswith("window "):
                tokens = line.split()
                window = Window(dict(zip(tokens[0::2], tokens[1::2])))
                profile.windows.append(window)
            elif line.startswith("thread ") and window is not None:
                tokens = line.split()
                stats = window.threads.setdefault(tokens[1], [0.0, 0, 0, 0])
                stats[0] += float(tokens[5])
                stats[1] += int(tokens[7])
                stats[2] += int(tokens[9])
                stats[3] += 1
            elif line.startswith("stack ") and window is not None:
                tokens = line.split(" ", 4)
                frames = tokens[4].split(";") if len(tokens) > 4 and tokens[4] else []
                window.stacks.append((tokens[1], tokens[2], int(tokens[3]), frames))
    return profile


class Symbolizer:
    def __init__(self, exe, module, full_names):
        self.resolver = Resolver(exe) if exe else None
        self.module = module.lower()
        self.full_names = full_names
        self.cache = {}

    def frame(self, token, leaf):
        """(display name, is it eden-uwp.exe code)."""
        key = (token, leaf)
        cached = self.cache.get(key)
        if cached is not None:
            return cached
        if token == "[jit]":
            result = (JIT, False)
        else:
            module, _, offset = token.partition("+0x")
            if module.lower() == self.module and offset and self.resolver:
                rva = int(offset, 16)
                # A return address points after its call: look the call up instead.
                name = self.resolver.symbol(rva if leaf else rva - 1)[0]
                name = name if name else token
                name = name if self.full_names else short_name(name)
                # MSVC spells anonymous namespaces `anonymous namespace', which breaks Markdown.
                result = (name.replace("`", "'"), True)
            elif module.lower() == self.module:
                result = (token, True)
            else:
                result = (f"[{module}]", False)
        self.cache[key] = result
        return result


def subsystem(names, is_eden):
    for name, eden in zip(reversed(names), reversed(is_eden)):
        if name == JIT:
            return "guest code (JIT)"
        if not eden:
            continue
        for pattern, label in SUBSYSTEMS:
            if pattern.search(name):
                return label
    leaf = names[-1] if names else "?"
    return "OS / driver" if leaf.startswith("[") else "other"


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


def select_windows(profile, spec):
    if not spec:
        return profile.windows
    first, _, last = spec.partition("-")
    first = int(first)
    last = int(last) if last else first
    return [w for w in profile.windows if first <= w.number <= last]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("profile")
    parser.add_argument("--exe", default=os.path.join("build-uwp", "bin", "eden-uwp.exe"))
    parser.add_argument("--window", help="window number or range (e.g. 3 or 2-5); all by default")
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--out", help="write the report here instead of printing it")
    parser.add_argument("--folded", help="also write folded stacks for a flame graph")
    parser.add_argument("--full-names", action="store_true", help="keep template arguments")
    parser.add_argument("--min-cpu", type=float, default=0.02,
                        help="skip threads below this share of a core (default 0.02)")
    args = parser.parse_args()

    profile = parse(args.profile)
    windows = select_windows(profile, args.window)
    if not windows:
        sys.exit("no window in the profile (or none in the range given)")
    out = open(args.out, "w", encoding="utf-8") if args.out else sys.stdout
    emit = lambda text="": print(text, file=out)  # noqa: E731

    exe = args.exe if os.path.exists(args.exe) else None
    if exe is None:
        emit(f"> {args.exe} not found: eden-uwp.exe frames stay as RVAs.")
    elif profile.timestamp is not None and pe_timestamp(exe) != profile.timestamp:
        emit(f"> WARNING: {exe} (time stamp 0x{pe_timestamp(exe):08x}) is not the build that "
             f"wrote the profile (0x{profile.timestamp:08x}): function names will be wrong.")
    symbols = Symbolizer(exe, os.path.basename(args.exe), args.full_names)

    # Totals over the chosen windows.
    seconds = sum(w.seconds for w in windows)
    hz = windows[0].hz
    threads = collections.OrderedDict()  # name -> [cpu_ms, running, waiting, instances]
    for window in windows:
        for name, stats in window.threads.items():
            total = threads.setdefault(name, [0.0, 0, 0, 0])
            for i in range(3):
                total[i] += stats[i]
            total[3] = max(total[3], stats[3])

    # Per thread aggregates.
    self_time = collections.defaultdict(collections.Counter)       # running, leaf function
    inclusive = collections.defaultdict(collections.Counter)       # running, any frame
    os_callers = collections.defaultdict(collections.Counter)      # running, leaf outside eden
    buckets = collections.defaultdict(collections.Counter)         # running, subsystem
    leaf_modules = collections.defaultdict(collections.Counter)    # running, leaf module
    wait_sites = collections.defaultdict(collections.Counter)      # waiting, eden callers
    hot_paths = collections.defaultdict(collections.Counter)       # running, last frames
    blocked_between = collections.Counter()                        # R samples caught waiting
    folded = collections.Counter()
    for window in windows:
        for thread, state, count, frames in window.stacks:
            names, is_eden = [], []
            for index, token in enumerate(frames):
                name, eden = symbols.frame(token, index == len(frames) - 1)
                names.append(name)
                is_eden.append(eden)
            if args.folded:
                folded[";".join([f"{thread} ({'running' if state == 'R' else 'waiting'})"] +
                                [n.replace(";", ":") for n in names])] += count
            if not names:
                continue
            eden_chain = [n for n, e in zip(names, is_eden) if e]
            if state == "R" and OS_MODULE.match(names[-1]) and eden_chain and                     WAIT_FUNCTIONS.search(eden_chain[-1]):
                state = "W"  # woke and blocked again between samples; caught in the wait
                blocked_between[thread] += count
            if state == "W":
                site = " <- ".join(reversed(eden_chain[-3:])) if eden_chain else names[-1]
                wait_sites[thread][site] += count
                continue
            leaf = names[-1]
            self_time[thread][leaf] += count
            # Own code only: every stack passes through the thread start in the OS's modules.
            for name in set(n for n, e in zip(names, is_eden) if e or n == JIT):
                inclusive[thread][name] += count
            leaf_modules[thread][os.path.basename(args.exe) if is_eden[-1] else leaf] += count
            if not is_eden[-1] and leaf != JIT and eden_chain:
                os_callers[thread][f"{eden_chain[-1]} -> {leaf}"] += count
            buckets[thread][subsystem(names, is_eden)] += count
            hot_paths[thread][" > ".join(names[-5:])] += count

    def cpu_share(name):
        return threads[name][0] / 1000.0 / seconds if seconds else 0.0

    ordered = sorted(threads, key=lambda name: -threads[name][0])
    shown = [name for name in ordered if cpu_share(name) >= args.min_cpu]

    first, last = windows[0], windows[-1]
    emit(f"# Sampling profile: windows {first.number}-{last.number}, {seconds:.1f} s at {hz} Hz")
    emit()
    overhead_ms = sum(float(w.meta.get("sampler_cpu_ms", 0)) for w in windows)
    suspended = [float(w.meta.get("suspended_us_avg", 0)) for w in windows]
    dropped = sum(int(w.meta.get("dropped_samples", 0)) for w in windows)
    emit(f"Profiler cost: its thread used {pct(overhead_ms / 1000.0, seconds):.1f}% of a core; "
         f"each sample paused its thread {sum(suspended) / len(suspended):.1f} us on average"
         + (f"; {dropped} samples past the stack limit were not kept" if dropped else "") + ".")
    emit()

    # Verdict.
    emit("## Verdict")
    emit()
    saturated = [name for name in shown if cpu_share(name) >= SATURATED]
    if saturated:
        emit("Threads busy at least 85% of a core: they set the pace. Their time goes to:")
        emit()
        for name in saturated:
            running = sum(buckets[name].values())
            top = ", ".join(f"{label} {pct(n, running):.0f}%"
                            for label, n in buckets[name].most_common(4))
            emit(f"- **{name}** ({pct(cpu_share(name), 1):.0f}% of a core): {top}")
    else:
        emit("No thread is busy 85% of a core or more: the frame waits between threads (see each "
             "thread's wait sites) or on the GPU.")
    emit()

    emit("## CPU time per thread")
    emit()
    emit("| Thread | Instances | CPU (% of a core) | CPU ms/s | Running samples | Waiting samples |")
    emit("| --- | --- | --- | --- | --- | --- |")
    for name in ordered:
        cpu_ms, running, waiting, instances = threads[name]
        if cpu_share(name) < args.min_cpu and running == 0:
            continue
        emit(f"| {name} | {instances} | {pct(cpu_share(name), 1):.1f}% | "
             f"{cpu_ms / seconds:.1f} | {running} | {waiting} |")
    emit()

    for name in shown:
        running = sum(self_time[name].values())
        waiting = sum(wait_sites[name].values())
        cpu_ms_per_s = threads[name][0] / seconds
        to_ms = lambda n: cpu_ms_per_s * n / running if running else 0.0  # noqa: E731
        emit(f"## {name}: {pct(cpu_share(name), 1):.1f}% of a core")
        emit()
        emit("CPU ms/s estimates split the thread's CPU time by its running samples"
             + (f" ({blocked_between[name]} samples that ran but were caught blocked count as "
                "waits)" if blocked_between[name] else "") + ".")
        emit()
        if running:
            emit("**Subsystems**")
            emit()
            emit("| Subsystem | Share | CPU ms/s |")
            emit("| --- | --- | --- |")
            for label, n in buckets[name].most_common():
                emit(f"| {label} | {pct(n, running):.1f}% | {to_ms(n):.2f} |")
            emit()
            emit("**Where the leaf runs**")
            emit()
            emit(", ".join(f"{module} {pct(n, running):.1f}%"
                           for module, n in leaf_modules[name].most_common(8)))
            emit()
            emit(f"**Hottest functions, own time** (top {args.top})")
            emit()
            emit("| Function | Self | CPU ms/s |")
            emit("| --- | --- | --- |")
            for function, n in self_time[name].most_common(args.top):
                emit(f"| `{function}` | {pct(n, running):.1f}% | {to_ms(n):.2f} |")
            emit()
            emit(f"**Hottest functions, with what they call** (top {args.top})")
            emit()
            emit("| Function | Inclusive | CPU ms/s |")
            emit("| --- | --- | --- |")
            for function, n in inclusive[name].most_common(args.top):
                emit(f"| `{function}` | {pct(n, running):.1f}% | {to_ms(n):.2f} |")
            emit()
            if os_callers[name]:
                emit("**Calls into the OS and the driver** (who calls them)")
                emit()
                emit("| Caller -> module | Share | CPU ms/s |")
                emit("| --- | --- | --- |")
                for site, n in os_callers[name].most_common(12):
                    emit(f"| `{site}` | {pct(n, running):.1f}% | {to_ms(n):.2f} |")
                emit()
            emit("**Hot paths** (last five frames)")
            emit()
            for path, n in hot_paths[name].most_common(10):
                emit(f"- {pct(n, running):.1f}%: `{path}`")
            emit()
        if waiting:
            emit(f"**Where it waits** ({pct(waiting, waiting + running):.0f}% of its samples)")
            emit()
            for site, n in wait_sites[name].most_common(8):
                emit(f"- {pct(n, waiting):.1f}%: `{site}`")
            emit()

    if args.folded:
        with open(args.folded, "w", encoding="utf-8") as file:
            for stack, count in folded.most_common():
                file.write(f"{stack} {count}\n")
        emit(f"Folded stacks: {args.folded} (open it on https://www.speedscope.app).")
    if out is not sys.stdout:
        out.close()


if __name__ == "__main__":
    main()

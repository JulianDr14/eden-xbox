#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract comparable complete perf windows, not hitch reports or partial shutdown totals."""

import argparse
import json
import re
from pathlib import Path


def windows(path):
    current = None
    with Path(path).open(encoding="utf-8", errors="replace") as source:
        for line in source:
            perf = re.search(r"D3D12 perf over (\d+) frames \((\d+) ms\)", line)
            if perf:
                current = {"frames": int(perf[1]), "elapsed_ms": int(perf[2])}
                stamp = re.search(r"\[\s*([\d.]+)\]", line)
                current["end_seconds"] = float(stamp[1]) if stamp else None
                for key, pattern in {
                    "draws": r"\| (\d+) draws,",
                    "gpu_busy_ms": r"GPU busy ([\d.]+) ms",
                    "pipeline_wait_ms": r"pipeline stalls \d+ \(([\d.]+) ms\)",
                    "idle_ms": r"GPU thread: idle ([\d.]+) ms",
                }.items():
                    match = re.search(pattern, line)
                    if match:
                        current[key] = float(match[1]) if key != "draws" else int(match[1])
            detail = re.search(r"D3D12 GPU thread: draws ([\d.]+) ms \(([\d.]+) us each:", line)
            disabled = "D3D12 GPU thread: detailed per-draw profiling off" in line
            if current is not None and (detail or disabled) and current.get("draws"):
                current["draw_ms"] = float(detail[1]) if detail else None
                current["draw_us"] = current["draw_ms"] * 1000 / current["draws"] if detail else None
                current["draws_per_frame"] = current["draws"] / current["frames"]
                current["draw_ms_per_frame"] = current["draw_ms"] / current["frames"] if detail else None
                current["fps"] = current["frames"] * 1000 / current["elapsed_ms"]
                for phase in ("textures", "buffers", "descriptors", "targets", "samplers", "record"):
                    match = re.search(rf"\b{phase} ([\d.]+)", line)
                    if match:
                        current[phase + "_us_rounded"] = float(match[1])
                yield current
                current = None


def summarize(rows):
    frames = sum(row["frames"] for row in rows)
    draws = sum(row["draws"] for row in rows)
    elapsed = sum(row["elapsed_ms"] for row in rows)
    profiled = [row for row in rows if row["draw_ms"] is not None]
    draw_ms = sum(row["draw_ms"] for row in profiled)
    measured_draws = sum(row["draws"] for row in profiled)
    measured_frames = sum(row["frames"] for row in profiled)
    return {
        "windows": len(rows), "frames": frames, "draws": draws,
        "draw_profiled_windows": len(profiled),
        "draw_us_weighted": draw_ms * 1000 / measured_draws if measured_draws else None,
        "draw_ms_per_frame": draw_ms / measured_frames if measured_frames else None,
        "draws_per_frame": draws / frames if frames else None,
        "fps": frames * 1000 / elapsed if elapsed else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+")
    parser.add_argument("--min-draws-per-frame", type=float, default=0)
    parser.add_argument("--after-seconds", type=float, default=0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    reports = []
    for path in args.logs:
        rows = [row for row in windows(path)
                if row["draws_per_frame"] >= args.min_draws_per_frame
                and (row["end_seconds"] or 0) - row["elapsed_ms"] / 1000 >= args.after_seconds]
        reports.append({"path": str(path), "windows": rows, "summary": summarize(rows)})
    if args.json:
        print(json.dumps(reports, indent=2))
    else:
        for report in reports:
            print(report["path"])
            print("end_s  frames  draws/frame  draw_us  draw_ms/frame  FPS  pipeline_wait_ms")
            for row in report["windows"]:
                draw_us = f'{row["draw_us"]:.3f}' if row["draw_us"] is not None else "--"
                draw_frame = f'{row["draw_ms_per_frame"]:.3f}' if row["draw_ms_per_frame"] is not None else "--"
                print(f'{row["end_seconds"] or 0:.2f} {row["frames"]:7d} '
                      f'{row["draws_per_frame"]:12.1f} {draw_us:>8} '
                      f'{draw_frame:>14} {row["fps"]:6.2f} '
                      f'{row.get("pipeline_wait_ms", 0):16.1f}')
            print(json.dumps(report["summary"]))
        print("Inclusive elapsed scopes, not CPU busy. Scene/configuration/compilation equivalence must be checked separately.")


if __name__ == "__main__":
    main()

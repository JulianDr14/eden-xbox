#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import importlib.util
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "draw_report", Path(__file__).parents[1] / "draw-preparation-report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

fixture = """
[ 10.0] D3D12 perf over 300 frames (5000 ms) | GPU thread: idle 100.0 ms, pipeline stalls 0 (0.0 ms) | GPU busy 200.0 ms | 600000 draws, 0 dispatches
[ 10.0] D3D12 GPU thread: draws 1200.0 ms (2.0 us each: textures 0.1, buffers 0.2, descriptors 0.3, targets 0.4, samplers 0.1, record 0.3)
[ 11.0] D3D12 hitch detail: draws 400.0 ms (2.0 us each: textures 0.1)
[ 15.0] D3D12 perf over 300 frames (10000 ms) | GPU thread: idle 200.0 ms, pipeline stalls 2 (7.0 ms) | GPU busy 800.0 ms | 300000 draws, 1 dispatches
[ 15.0] D3D12 GPU thread: draws 900.0 ms (3.0 us each: textures 0.2, buffers 0.3, descriptors 0.4, targets 0.5, samplers 0.2, record 0.4)
[ 20.0] D3D12 perf over 300 frames (7000 ms) | 900000 draws, 0 dispatches
[ 20.0] D3D12 GPU thread: detailed per-draw profiling off
[ 25.0] D3D12 perf over 300 frames (7000 ms) | 900000 draws, 0 dispatches
"""
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "fixture.txt"
    path.write_text(fixture, encoding="utf-8")
    rows = list(report.windows(path))
    assert len(rows) == 3  # normal windows retain FPS without inventing Draw CPU time
    assert rows[0]["draw_us"] == 2 and rows[1]["draw_us"] == 3
    assert rows[0]["draw_ms_per_frame"] == 4
    assert rows[1]["pipeline_wait_ms"] == 7
    summary = report.summarize(rows[:2])
    assert abs(summary["draw_us_weighted"] - 7 / 3) < 1e-12
    assert summary["fps"] == 40  # time-weighted, not mean(60, 30)
    assert report.summarize([])["draw_us_weighted"] is None
    assert rows[2]["draw_ms"] is None and rows[2]["draw_us"] is None
    assert rows[2]["fps"] == 300000 / 7000
    assert report.summarize(rows[2:])["draw_us_weighted"] is None
    assert report.summarize(rows)["draw_profiled_windows"] == 2
    assert report.summarize(rows)["draw_us_weighted"] == summary["draw_us_weighted"]
print("PASS: normal/profiled complete windows, missing CPU is null, hitches/partial excluded, weighted costs/FPS")

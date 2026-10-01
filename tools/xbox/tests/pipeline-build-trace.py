# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pipeline identity, nested phases, collision-free lifetime handling and capture borders."""
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "trace", Path(__file__).resolve().parents[1] / "analyze-frame-trace.py")
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)

events = [
    (0., 1, "game-queue-buffer", 0, 1),
    (1., 1, "pipeline-build-requested", 0x1234, 0x99),
    (2., 1, "pipeline-frontend-long", 1500, 0x99),
    (3., 2, "pipeline-worker-begin", 0x1234, 0),
    (10., 2, "pipeline-translate-long", 7000, 0x1234),
    (11., 2, "pipeline-sign-long", 1000, 0x1234),
    (12., 2, "pipeline-sign-long", 1000, 0x1234),
    (12., 2, "pipeline-cache-result", 0, 0x1234),
    (12., 2, "pipeline-dxil-long", 9000, 0x1234),
    (16., 2, "pipeline-pso-long", 4000, 0x1234),
    (16., 2, "pipeline-build-done", 0x1234, 1),
    (16.1, 2, "pipeline-worker-long", 13100, 0x1234),
    (16.2, 1, "pipeline-wait-long", 14200, 0x1234),
    (20., 1, "game-queue-buffer", 0, 1),
    # Reused pointer has a new lifetime; previous row remains intact.
    (21., 1, "pipeline-build-requested", 0x1234, 0x100),
    (22., 2, "pipeline-worker-begin", 0x1234, 0),
    (22.1, 2, "pipeline-cache-result", 1, 0x1234),
    (23., 2, "pipeline-build-done", 0x1234, 1),
    # Worker began before T: no invented queue delay.
    (24., 2, "pipeline-cache-result", 2, 0x5678),
    (25., 2, "pipeline-dxil-long", 1000, 0x5678),
    (25., 2, "pipeline-build-done", 0x5678, 0),
]
rows, frontend = trace.pipeline_chains(events)
assert len(rows) == 3 and len(frontend) == 1
first, second, border = rows
assert first["key_hash"] == "0x99" and second["key_hash"] == "0x100"
assert first["queue_to_worker_ms"] == 2 and first["request_to_done_ms"] == 15
assert first["phase_union_ms"]["pipeline-sign-long"] == 2
assert first["phase_union_ms"]["pipeline-dxil-long"] == 9
assert first["phase_union_ms"]["pipeline-wait-long"] == 14.2
assert first["frontend"][0]["elapsed_ms"] == 1.5
assert first["cache_outcome"] == "compiled" and second["cache_outcome"] == "hit"
assert border["cache_outcome"] == "shared_in_flight" and not border["success"]
assert "queue_to_worker_ms" not in border and "request_to_done_ms" not in border
result = trace.analyze(events)
assert result["pipeline_cache_outcomes"] == {"compiled": 1, "hit": 1, "shared_in_flight": 1}
gap = result["longest_gaps"][0]
assert gap["pipeline_wait_overlap_ms"] == 14.2
assert gap["pipeline_frontend_overlap_ms"] == 1.5
assert result["fps_estimate"] is None
assert trace.pipeline_chains([]) == ([], [])
print("Pipeline fixtures passed: exact lifetime, nested phases, repeated signing, borders and gap overlap")

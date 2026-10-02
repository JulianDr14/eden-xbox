# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""GC nested phase union, host isolation, metadata and old trace compatibility."""
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('trace', Path(__file__).resolve().parents[1] / 'analyze-frame-trace.py')
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)
events = [
    (0., 1, 'texture-gc-image-info', 10*1048576, 99),
    (0., 1, 'texture-gc-image-format', 5, 99),
    (1., 1, 'texture-gc-sync-reason', 2, 99),
    (2., 1, 'texture-gc-staging', 1000, 99),
    (3., 1, 'texture-gc-copy', 1000, 99),
    (10., 2, 'texture-gc-wait', 9000, 100),  # Other host must be excluded.
    (12., 1, 'texture-gc-wait', 9000, 99),
    (12., 1, 'texture-gc-prepare', 12000, 99),
    (15., 1, 'texture-gc-swizzle', 3000, 99),
    (16., 1, 'texture-gc-release', 1000, 99),
    (18., 1, 'texture-gc-long', 18000, 7),
    (20., 1, 'texture-gc-long', 1000, 8),
]
first, second = trace.gc_breakdown(events)
assert first['phase_union_ms']['prepare'] == 12
assert first['phase_union_ms']['wait'] == 9
assert first['unattributed_ms'] == 3  # Nested prepare must not be summed.
assert first['phases'][0]['bytes'] == 10*1048576
assert first['phases'][0]['format_enum'] == 5
assert first['sync_reasons'][0]['reason'] == 2
assert not second['phases'] and not second['sync_reasons']
assert second['unattributed_ms'] == 1  # Old logs have no breakdown.
# A phase crossing the outer interval is clipped, with overlap counted once.
row = trace.gc_breakdown([(6., 1, 'texture-gc-wait', 6000, 9),
                          (7., 1, 'texture-gc-wait', 2000, 9),
                          (8., 1, 'texture-gc-long', 4000, 1)])[0]
assert row['phase_union_ms']['wait'] == 3 and row['unattributed_ms'] == 1
print('PASS GC nested union, same-host, metadata, borders and old logs')
compact = trace.gc_breakdown([(3., 1, 'texture-gc-compact', 2000, 9), (4., 1, 'texture-gc-prepare', 4000, 9), (5., 1, 'texture-gc-long', 5000, 1)])[0]
assert compact['phase_union_ms']['compact'] == 2 and compact['unattributed_ms'] == 3

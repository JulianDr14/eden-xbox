# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
from pathlib import Path
import struct
import tempfile

spec = importlib.util.spec_from_file_location(
    'reflections', Path(__file__).parents[1]/'analyze-material-reflections.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    prefix = 'material_fixture_1_draw_44'
    paths = {}
    for channel in probe.CHANNELS:
        path = root/(prefix+'_gpu_reflection_'+channel+'_x10_y20.raw')
        path.write_bytes(struct.pack('<8f', 1, 2, 3, .25, *([-99999]*4)))
        path.with_suffix('.json').write_text(json.dumps(dict(width=2, height=1,
            row_bytes=32, dxgi_format=2, native_bytes=True)), encoding='utf8')
        paths[channel] = path
    result = probe.summarize(root, prefix)
    assert result['finite_covered'] == 1 and result['uncovered'] == 1
    assert result['lod_range'] == [.25, .25]
    assert result['implicit_vs_level_zero_max'] == 0
    final = root/(prefix+'_gpu_reflection_original_final_x10_y20.raw')
    final.write_bytes(struct.pack('<8f', .125, .25, .5, 1, *([-99999]*4)))
    final.with_suffix('.json').write_text(json.dumps(dict(width=2, height=1,
        row_bytes=32, dxgi_format=2, native_bytes=True)), encoding='utf8')
    packed = root/(prefix+'_gpu_reflection_original_r11_x10_y20.raw')
    packed.write_bytes(struct.pack('<II', 0x300 | (0x340 << 11) | (0x1c0 << 22), 0))
    packed.with_suffix('.json').write_text(json.dumps(dict(width=2, height=1,
        row_bytes=8, dxgi_format=26, native_bytes=True)), encoding='utf8')
    comparison = probe.summarize(root, prefix)['original_format_comparison']
    assert comparison['finite_pixels'] == 1 and comparison['float_vs_r11_max_delta'] == 0
    gradient = paths['gradient_x']
    gradient.write_bytes(struct.pack('<8f', 1, 2, 3, .25, 1, 2, 3, 4))
    assert probe.summarize(root, prefix)['inconsistent_coverage'] == 1
    gradient.write_bytes(struct.pack('<8f', 1, 2, 3, .25, *([-99999]*4)))
    sample = paths['implicit']
    sample.write_bytes(struct.pack('<8f', 1, 2, 3, .5, *([-99999]*4)))
    assert probe.summarize(root, prefix)['implicit_vs_level_zero_max'] == .25
    sample.write_bytes(struct.pack('<8f', float('nan'), 2, 3, .5, *([-99999]*4)))
    assert probe.summarize(root, prefix)['nonfinite'] == 1
    sample.write_bytes(b'bad')
    try:
        probe.summarize(root, prefix)
    except ValueError:
        pass
    else:
        raise AssertionError('Truncated probe accepted')
    sample.write_bytes(struct.pack('<8f', 1, 2, 3, .25, *([-99999]*4)))
    renamed = sample.with_name(sample.name.replace('_x10_', '_x11_'))
    sample.rename(renamed)
    sample.with_suffix('.json').rename(renamed.with_suffix('.json'))
    try:
        probe.summarize(root, prefix)
    except ValueError:
        pass
    else:
        raise AssertionError('Mismatched ROI accepted')
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    for channel in probe.CFG_CHANNELS:
        path = root/(prefix+'_gpu_reflection_'+channel+'_x10_y20.raw')
        path.write_bytes(struct.pack('<4f', .25, .5, .75, 1))
        path.with_suffix('.json').write_text(json.dumps(dict(width=1, height=1,
            row_bytes=16, dxgi_format=2, native_bytes=True)))
    result = probe.summarize(root, prefix)
    assert result['mode'] == 'original_cfg' and result['finite_covered'] == 1
    assert result['lod_range'] is None and result['implicit_vs_level_zero_max'] is None
    assert result['ranges']['cfg_cube'][0] == [.25, .25]
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    for channel in probe.BRANCH_CHANNELS:
        path = root/(prefix+'_gpu_reflection_'+channel+'_x10_y20.raw')
        path.write_bytes(struct.pack('<4f', .25, .5, .75, 1))
        path.with_suffix('.json').write_text(json.dumps(dict(width=1, height=1,
            row_bytes=16, dxgi_format=2, native_bytes=True)))
    result = probe.summarize(root, prefix)
    assert result['mode'] == 'branch_controls' and result['finite_covered'] == 1
    assert result['lod_range'] is None and result['implicit_vs_level_zero_max'] is None
print('PASS reflection probes: authored/CFG/branches, finite values, sentinel, LOD, sampling delta, NaN, truncation, ROI mismatch')

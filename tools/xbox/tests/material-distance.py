# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('distance',
    Path(__file__).resolve().parents[1] / 'compare-material-distance.py')
distance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(distance)


def capture(pointer='0x123', sampler='lod 0..9', vertices=3, complete=True,
            state='fetch5[dxgi=42] interp[5:0;] bounds[guest=0 host=0]', ps='456'):
    lines = ['D3D12 material frame 42: begin',
             f'D3D12 trace #0 samplers: {sampler}',
             f'D3D12 trace #0: draw ({vertices} vertices x1) | VS 123 PS {ps} '
             f'PSO={pointer} key=abcd {state}',
             'D3D12 trace #1 samplers_ref=#0',
             f'D3D12 trace #1: draw ({vertices} vertices x1) state_ref=#0']
    if complete:
        lines += ['D3D12 material complete: matched=2 selected=2 bytes=100 '
                  'omitted_draws=0 dropped_lines=0']
    return distance.material.captures(lines)[0]


base = capture()
result = distance.compare([('near', base), ('middle', capture(pointer='0x456')),
                           ('far', capture(pointer='0x789'))])
assert result['comparable_complete'] and not result['differences']
for changed in (capture(sampler='lod 2..9'), capture(vertices=6),
                capture(state='fetch5[dxgi=24] interp[5:1;] bounds[guest=1 host=0]'),
                capture(ps='789')):
    result = distance.compare([('near', base), ('far', changed)])
    assert result['comparable_complete'] and result['differences']
assert not distance.compare([('near', base), ('far', capture(complete=False))])['comparable_complete']
assert distance.variants(base)[('123', '456')]['draw_count'] == 2
platforms = distance.material.captures([
    "D3D12: adapter 'PC'", 'D3D12: wave ops yes (lanes 32-64)',
    'D3D12 material frame 1: begin',
    'D3D12 material complete: matched=0 selected=0 bytes=0 omitted_draws=0 dropped_lines=0',
    "D3D12: adapter 'SraKmd_arden'", 'D3D12: wave ops yes (lanes 64-64)',
    'D3D12 material frame 2: begin',
    'D3D12 material complete: matched=0 selected=0 bytes=0 omitted_draws=0 dropped_lines=0'])
report = distance.compare([('PC', platforms[0]), ('Series', platforms[1])])
assert report['samples'][0]['capabilities']['adapter'] == "'PC'"
assert report['samples'][1]['capabilities']['wave ops'] == 'yes (lanes 64-64)'
try:
    distance.load_sample('no-label')
    raise AssertionError('invalid syntax accepted')
except ValueError:
    pass
print('PASS distance comparisons: refs, COM identity ignored, sampler/LOD/vertex/fetch/'
      'interpolation/depth/shader differences, partial captures')

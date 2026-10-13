# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import sys

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('material', Path(__file__).resolve().parents[1] / 'analyze-material-trace.py')
material = importlib.util.module_from_spec(spec)
spec.loader.exec_module(material)

lines = [
    'D3D12 material frame 42: begin (metadata only)',
    'D3D12 trace #0 samplers: srv=[@abc null=true] sampler=[filter 0x14]',
    'D3D12 trace #0: draw (3 vertices x1) | VS 123 PS 456 | tex 25 x',
    'D3D12 trace #1 samplers_ref=#0',
    'D3D12 trace #1: draw (6 vertices x1) state_ref=#0',
    'D3D12 material complete: matched=2 selected=2 bytes=100 omitted_draws=0 dropped_lines=0',
]
capture = material.captures(lines)[0]
assert capture['draws'][1]['bindings'] == capture['draws'][0]['bindings']
assert '(6 vertices' in capture['draws'][1]['state']
assert capture['draws'][1]['state'].endswith(' | VS 123 PS 456 | tex 25 x')
summary = material.summarize(capture)
assert not summary['partial'] and summary['parsed_draws'] == 2
assert summary['shaders']['456']['draws'] == 2 and len(summary['null_srvs']) == 2
assert material.summarize(material.captures(lines[:-1])[0])['partial']
bad = lines.copy()
bad[4] = 'D3D12 trace #1: draw (6 vertices x1) state_ref=#99'
assert material.summarize(material.captures(bad)[0])['partial']
bad[4] = 'D3D12 trace #1: draw (6 vertices x1) state_ref=#1'
assert material.captures(bad)[0]['errors']
bad = lines.copy()
bad[-1] = 'D3D12 material complete: matched=3 selected=2 bytes=100 omitted_draws=1 dropped_lines=1'
assert material.summarize(material.captures(bad)[0])['partial']
assert len(material.captures(lines + lines)) == 2
bad = lines.copy()
bad[-1] += ' intervals_complete=0'
assert material.summarize(material.captures(bad)[0])['partial']
assert not material.captures(['D3D12 trace #0: regular old trace'])
print('PASS material parser: state/binding references, changed vertex counts, boundaries, missing/forward refs, partial captures')

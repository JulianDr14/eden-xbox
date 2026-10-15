#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Distinguish two objects sharing VS/PS; never attribute the first pair to both."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

tool = Path(__file__).parents[1]/'project-captured-mesh.py'
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    trace = root/'trace'
    trace.mkdir()
    prefix = 'material_fixture_1'
    (trace/(prefix+'_probe.csv')).write_text('draw,target\n',encoding='utf8')
    immediate = lambda value: dict(type='F32',bits=struct.unpack('<I',struct.pack('<f',value))[0])
    instructions = []
    for component in range(4):
        if component < 3:
            n = len(instructions)
            instructions.append(dict(id=n,op='GetAttribute',args=[dict(value=32+component),dict(value=0)]))
            value = dict(ref=n)
        else:
            value = immediate(1)
        instructions.append(dict(id=len(instructions),op='SetAttribute',
                                 args=[dict(value=28+component),value,dict(value=0)]))
    ir = root/'vs.json'
    ir.write_text(json.dumps(dict(hash='aa',straight_line=True,cbuf_descriptors=[],
                                  blocks=[instructions])),encoding='utf8')
    log = ['D3D12 material frame 1: begin']
    for draw, offset in ((1,-.5),(2,.5)):
        log.append(f'D3D12 trace #{draw}: draw indexed (3 vertices x1) | VS aa PS bb fetch0[dxgi=6 fallback=false offset=0 stride=12]')
        log.append(f'D3D12 material inputs: draw={draw} vs_aa_ps_bb vertices=3 instances=1 first_index=0 base_vertex=0 base_instance=0 indexed=true')
        stem = f'{prefix}_draw_{draw}_vs_aa_ps_bb'
        (trace/(stem+'_binding_0_kind_3_slot_0.bin')).write_bytes(b''.join(
            struct.pack('<3f',x+offset,y,0) for x,y in ((-.1,-.1),(.1,-.1),(0,.1))))
        (trace/(stem+'_binding_1_kind_4_slot_0.bin')).write_bytes(struct.pack('<3H',0,1,2))
    log.append('D3D12 material complete: matched=2 selected=2 omitted_draws=0 dropped_lines=0 intervals_complete=1')
    (root/'eden_log.txt').write_text('\n'.join(log),encoding='utf8')
    bounds = []
    for draw in (None,2):
        output = root/f'mesh-{draw}.json'
        command = [sys.executable,str(tool),str(root),str(ir),'bb','--capture','1','--output',str(output)]
        if draw is not None:
            command += ['--draw',str(draw)]
        subprocess.run(command,check=True,capture_output=True)
        report = json.loads(output.read_text(encoding='utf8'))
        assert report['draw'] == (draw or 1)
        bounds.append(report['screen_bounds'])
    assert bounds[0][2] < bounds[1][0], 'Different objects were conflated'
    failed = subprocess.run(command[:-2]+['--draw','99'],capture_output=True)
    assert failed.returncode != 0
print('PASS exact draw selection, first-pair compatibility, distinct projected objects, absent draw rejection')

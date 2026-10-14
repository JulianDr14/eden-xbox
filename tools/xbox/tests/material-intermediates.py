#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import json
import math
from pathlib import Path
import struct
import tempfile

spec = importlib.util.spec_from_file_location('intermediates', Path(__file__).parents[1]/'analyze-material-intermediates.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def export(name, data, width, height, fmt, row):
        path = root/name
        path.write_bytes(data)
        path.with_suffix('.json').write_text(json.dumps(dict(width=width,height=height,
            dxgi_format=fmt,row_bytes=row,native_bytes=True)),encoding='utf8')
        return path
    varying = export('varying.raw',struct.pack('<4f',.5,.5,0,0),1,1,2,16)
    sample = export('sample.raw',struct.pack('<4f',.25,.5,1,128/255),1,1,2,16)
    depth = export('depth.raw',struct.pack('<2e',.25,.5),1,1,34,4)
    mask = export('mask_m0_l0.raw',bytes([255,0])+bytes(6),4,4,80,8)
    export('mask_m9_l0.raw',bytes([128,0])+bytes(6),1,1,80,8)
    camera = root/'camera.bin'
    camera.write_bytes(bytes(352)+struct.pack('<4f',1,1,0,0))
    check = lambda: probe.compare(varying,sample,depth,mask,camera,0,0)
    result = check()
    assert result['covered']==1 and result['nonfinite']==0 and not any(result['points_above_tolerance'])
    control = lambda: probe.compare_varying_control(varying,camera,1,1,0,0)
    assert control()['covered']==1 and not any(control()['points_above_tolerance'])
    varying.write_bytes(struct.pack('<4f',.6,.5,0,0))
    assert check()['points_above_tolerance'][0]==1
    assert control()['points_above_tolerance'][0]==1
    varying.write_bytes(struct.pack('<4f',math.nan,.5,0,0))
    assert check()['nonfinite']==1 and check()['covered']==0
    assert control()['nonfinite']==1
    varying.write_bytes(struct.pack('<4f',-99999,.5,0,0))
    assert check()['covered']==0
    varying.write_bytes(bytes(15))
    try: check()
    except ValueError: pass
    else: raise AssertionError('truncated ROI accepted')
print('PASS intermediate probe parser: exact controls, coordinate mismatch, NaN, uncovered sentinel, truncated ROI')

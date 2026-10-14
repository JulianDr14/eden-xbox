# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic regression fixtures; no commercial shader data required."""
import importlib.util
from pathlib import Path
import struct
import math
import json
import tempfile

spec = importlib.util.spec_from_file_location('replay', Path(__file__).resolve().parents[1] / 'replay-lighting-ir.py')
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)
ir = replay.ir


def immediate(value):
    return dict(type='F32', bits=ir.float_bits(value))


def inst(n, op, args):
    return dict(id=n, op=op, args=args, flags=0)


for op, left, right, expected in (
        ('FPOrdGreaterThanEqual32', 1., 1., True),
        ('FPOrdGreaterThanEqual32', -1., 0., False),
        ('FPOrdGreaterThanEqual32', math.nan, 0., False),
        ('FPOrdEqual32', -0., 0., True),
        ('FPOrdEqual32', math.nan, math.nan, False)):
    program = dict(straight_line=True, blocks=[[inst(0, op, [immediate(left), immediate(right)])]])
    assert ir.evaluate(program, {}, {})[1][0] is expected


a, b = 1 + 2 ** -23, 1 - 2 ** -23
program = dict(straight_line=True, blocks=[[
    inst(0, 'FPFma32', [immediate(a), immediate(b), immediate(-1)]),
    inst(1, 'SetAttribute', [dict(value=28), dict(ref=0), dict(value=0)])]])
assert ir.evaluate(program, {}, {})[0][28] == -(2 ** -46)
assert ir.evaluate(program, {}, {}, fused=False)[0][28] == 0
for op, expected in (('FPSqrt', 2.0), ('FPRecipSqrt32', .5)):
    expression = dict(straight_line=True, blocks=[[inst(0, op, [immediate(4)])]])
    assert ir.evaluate(expression, {}, {})[1][0] == expected
    expression['blocks'][0][0]['args'] = [immediate(-1)]
    assert math.isnan(ir.evaluate(expression, {}, {})[1][0])
for invalid in (dict(program, straight_line=False), {'blocks': program['blocks']}):
    try:
        ir.evaluate(invalid, {}, {})
        raise AssertionError('Control-flow proof must be mandatory')
    except ValueError:
        pass
branch = dict(program, blocks=[[inst(0, 'ConditionRef', [immediate(1)])]])
try:
    ir.evaluate(branch, {}, {})
    raise AssertionError('Unsupported branch instruction must fail')
except ValueError:
    pass
slice_source = dict(straight_line=True, blocks=[[
    inst(0, 'Identity', [immediate(4)]),
    inst(1, 'ImageSampleImplicitLod', []),  # unused material sampling
    inst(2, 'SetAttribute', [dict(value=28), dict(ref=0), dict(value=0)]),
    inst(3, 'SetAttribute', [dict(value=32), dict(ref=1), dict(value=0)])]])
assert ir.evaluate(ir.attribute_slice(slice_source, {28}), {}, {})[0] == {28: 4}
try:
    ir.attribute_slice(slice_source, {29})
    raise AssertionError('Missing output must fail')
except ValueError:
    pass
# Comparison happens before filtering: alternating depths produce 0.5.
depth = struct.pack('<4H', 0, 65535, 0, 65535)
assert ir.compare_depth_bilinear(depth, 2, 2, (.5, .5), .5) == .5
assert ir.compare_depth_bilinear(depth, 2, 2, (.25, .25), 0) == 1
assert ir.compare_depth_bilinear(depth, 2, 2, (-10, -10), .5) == 1
assert ir.compare_depth_bilinear(depth, 2, 2, (.25, .25), .5, reverse=True) == 1
assert replay.pack_r11((1, 1, 1)) == (15 << 6) | ((15 << 6) << 11) | ((15 << 5) << 22)
assert replay.encode_small(1 + .75 / 64, 6, 'rne') == (15 << 6) + 1
assert replay.encode_small(1 + .75 / 64, 6, 'rtz') == (15 << 6)
control = replay.Capture.__new__(replay.Capture)
control.textures = [dict(dxgi=28, width=1, height=1, mapping=0x1688,
                        tile=dict(x=0, y=0, width=1, height=1, data=bytes((32, 64, 128, 112))))]
original_sample = control.sample(0, (.5, .5), None, None, {})
alpha_control = control.sample(0, (.5, .5), None, None, {'zero_albedo_alpha': True})
assert original_sample[:3] == alpha_control[:3] and alpha_control[3] == 0 and original_sample[3] > 0
control.textures = [{}, {'dxgi': 26}]
assert control.sample(1, (.5, .5), None, None, {'zero_lighting_input': True}) == (0, 0, 0, 1)
control.textures[1]['dxgi'] = 28
try:
    control.sample(1, (.5, .5), None, None, {'zero_lighting_input': True})
    raise AssertionError('Lighting control must reject unrelated input format')
except ValueError:
    pass
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'input.raw'
    info = dict(width=2, height=1, row_bytes=8, dxgi_format=33,
                native_bytes=True, origin='top-left')
    path.write_bytes(struct.pack('<4e', .25, .5, .75, 1))
    path.with_suffix('.json').write_text(json.dumps(info), encoding='utf-8')
    tile = replay.native_input_tile(path, '1234', 34)
    assert (tile['x'], tile['y'], tile['width'], tile['height'], tile['dxgi']) == (0, 0, 2, 1, 34)
    assert len(tile['data']) == 8
    for change in ({'native_bytes': False}, {'row_bytes': 16}, {'dxgi_format': 27}, {'origin': 'bottom-left'}):
        path.with_suffix('.json').write_text(json.dumps(dict(info, **change)), encoding='utf-8')
        try:
            replay.native_input_tile(path, '1234', 34)
            raise AssertionError('Incompatible input must fail')
        except ValueError:
            pass
    path.with_suffix('.json').write_text(json.dumps(info), encoding='utf-8')
    path.write_bytes(b'\0' * 7)
    try:
        replay.native_input_tile(path, '1234', 34)
        raise AssertionError('Truncated input must fail')
    except ValueError:
        pass
print('PASS scalar IR: fused rounding, fail-closed flow, dependency slice, PCF, packed RTV controls')

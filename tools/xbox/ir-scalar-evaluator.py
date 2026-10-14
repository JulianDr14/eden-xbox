# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail-closed scalar evaluator for straight-line captured IR, not a Maxwell emulator.

Sampling and attributes are supplied by the caller. Branches, memory writes and
unsupported instructions fail explicitly. Approximate reciprocal/sampling hardware
precision is not reproduced; callers must quantify that uncertainty.
"""
import ctypes
import math
import struct


def f32(value):
    try:
        return struct.unpack('<f', struct.pack('<f', value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def bits_float(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]


def float_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


try:
    _crt = ctypes.CDLL('ucrtbase' if __import__('os').name == 'nt' else 'libm.so.6')
    _fma = _crt.fmaf
    _fma.argtypes = (ctypes.c_float,) * 3
    _fma.restype = ctypes.c_float
except (OSError, AttributeError) as error:
    raise RuntimeError('A native correctly rounded fmaf is required') from error


def evaluate(program, cbufs, attributes, sample=None, *, fused=True):
    if program.get('straight_line') is not True:
        raise ValueError('Explicit straight-line control-flow proof required')
    values, outputs, trace = {}, {}, []

    def arg(value):
        if 'ref' in value:
            return values[value['ref']]
        if value.get('empty'):
            return None
        if 'bits' in value:
            if value['type'] != 'F32':
                raise ValueError('Unsupported immediate type: ' + value['type'])
            return bits_float(value['bits'])
        return value['value']

    for block in program['blocks']:
        for inst in block:
            op = inst['op']
            a = [arg(v) for v in inst['args']]
            if op in ('Prologue', 'Epilogue'):
                continue
            if op == 'Identity': value = a[0]
            elif op == 'GetCbufF32': value = struct.unpack_from('<f', cbufs[a[0]], a[1])[0]
            elif op == 'GetCbufU32x2': value = struct.unpack_from('<II', cbufs[a[0]], a[1])
            elif op == 'GetAttribute': value = attributes[a[0]]
            elif op == 'SetAttribute': outputs[a[0]] = a[1]; continue
            elif op == 'SetFragColor': outputs[a[0], a[1]] = a[2]; continue
            elif op == 'FPFma32':
                value = _fma(*a) if fused else f32(f32(a[0] * a[1]) + a[2])
            elif op == 'FPAdd32': value = f32(a[0] + a[1])
            elif op == 'FPMul32': value = f32(a[0] * a[1])
            elif op == 'FPNeg32': value = -a[0]
            elif op == 'FPAbs32': value = abs(a[0])
            elif op == 'FPRecip32': value = f32(1 / a[0]) if a[0] else math.copysign(math.inf, a[0])
            elif op == 'FPSqrt': value = f32(math.sqrt(a[0])) if a[0] >= 0 else math.nan
            elif op == 'FPRecipSqrt32':
                value = f32(1 / math.sqrt(a[0])) if a[0] > 0 else (
                    math.copysign(math.inf, a[0]) if a[0] == 0 else math.nan)
            elif op == 'FPSaturate32': value = min(1.0, max(0.0, a[0]))
            elif op == 'FPMin32': value = min(a)
            elif op == 'FPMax32': value = max(a)
            elif op == 'FPOrdLessThan32': value = a[0] < a[1]
            elif op == 'FPOrdGreaterThan32': value = a[0] > a[1]
            elif op == 'FPOrdGreaterThanEqual32': value = a[0] >= a[1]
            elif op == 'FPOrdEqual32': value = a[0] == a[1]
            elif op == 'SelectU32': value = a[1] if a[0] else a[2]
            elif op == 'BitCastF32U32': value = bits_float(a[0])
            elif op == 'BitCastU32F32': value = float_bits(a[0])
            elif op == 'IAdd32': value = (a[0] + a[1]) & 0xffffffff
            elif op == 'IMul32': value = (a[0] * a[1]) & 0xffffffff
            elif op.startswith('CompositeConstructF32x'): value = tuple(a)
            elif op.startswith('CompositeExtractF32x'): value = a[0][a[1]]
            elif op.startswith('CompositeExtractU32x'): value = a[0][a[1]]
            elif op in ('ImageSampleImplicitLod', 'ImageSampleDrefImplicitLod'):
                if sample is None: raise ValueError('Missing texture provider')
                reference = a[2] if 'Dref' in op else None
                value = sample(inst['flags'] & 0xffff, a[1], reference, inst)
            else: raise ValueError('Unsupported IR instruction: ' + op)
            values[inst['id']] = value
            trace.append((inst['id'], op, value))
    return outputs, values, trace


def attribute_slice(program, requested):
    """Backward slice for vertex position only; refuses non-linear programs.

    Excluding unused material sampling is valid only when the original control
    flow is straight-line. The result is for geometry attribution, not PS replay.
    """
    if program.get('straight_line') is not True:
        raise ValueError('Cannot slice control flow')
    instructions = [i for block in program['blocks'] for i in block]
    lookup = {i['id']: i for i in instructions}
    needed = set()

    def take(inst):
        if inst['id'] in needed:
            return
        needed.add(inst['id'])
        for value in inst['args']:
            if 'ref' in value:
                take(lookup[value['ref']])

    found = set()
    for inst in instructions:
        if inst['op'] == 'SetAttribute' and inst['args'][0].get('value') in requested:
            found.add(inst['args'][0]['value'])
            take(inst)
    if found != set(requested):
        raise ValueError('Requested outputs are missing')
    return dict(program, blocks=[[i for i in block if i['id'] in needed] for block in program['blocks']])


def compare_depth_bilinear(data, width, height, uv, reference, *, flip_y=False, reverse=False,
                           fraction_bits=None, force=None):
    """D16 LEQUAL comparison before bilinear filtering, with white border.

    The optional switches are one-variable offline experiments; never render state.
    """
    if force is not None: return force
    u, v = uv
    if flip_y: v = 1 - v
    x, y = u * width - 0.5, v * height - 0.5
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    if fraction_bits is not None:
        scale = 1 << fraction_bits
        fx, fy = round(fx * scale) / scale, round(fy * scale) / scale
    result = 0.0
    for dx, wx in ((0, 1 - fx), (1, fx)):
        for dy, wy in ((0, 1 - fy), (1, fy)):
            px, py = ix + dx, iy + dy
            depth = 1.0 if not (0 <= px < width and 0 <= py < height) else \
                struct.unpack_from('<H', data, (py * width + px) * 2)[0] / 65535.0
            passed = depth <= reference if reverse else reference <= depth
            result += wx * wy * passed
    return f32(result)

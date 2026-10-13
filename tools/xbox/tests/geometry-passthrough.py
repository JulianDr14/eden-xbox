# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the straight-line regression modules, poisoning GS outputs at EmitVertex.
This is a deliberately small semantic oracle, not a general SPIR-V implementation.
Unsupported executable instructions fail rather than being silently skipped.
"""
import copy
import pathlib
import struct
import sys


def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def unbits(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def run(path, poison=True, drop_restore=False):
    types, values, memory, pointers, decorations = {}, {}, {}, {}, {}
    outputs, emitted, passed = [], [], []
    in_function = False
    after_emit = False
    ended = 0
    for line in path.read_text().splitlines():
        token = line.split()
        if not token or token[0].startswith(';'):
            continue
        if len(token) > 2 and token[1] == '=':
            result, op, args = token[0], token[2], token[3:]
        else:
            result, op, args = None, token[0], token[1:]
        if op == 'OpDecorate':
            decorations.setdefault(args[0], {})[args[1]] = args[2:]
            if args[1] == 'PassthroughNV':
                passed.append(args[0])
        elif op.startswith('OpType'):
            types[result] = (op, args)
        elif op == 'OpConstant':
            values[result] = bits(float.fromhex(args[1]) if args[1].startswith('0x') else float(args[1])) if types[args[0]][0] == 'OpTypeFloat' else int(args[1])
        elif op == 'OpConstantTrue':
            values[result] = 1
        elif op == 'OpConstantFalse':
            values[result] = 0
        elif op == 'OpConstantComposite':
            values[result] = [values[x] for x in args[1:]]
        elif op == 'OpVariable':
            pointers[result] = (result, [])
            if args[1] == 'Input':
                # Distinct per-vertex and per-component sentinels, including location 31.
                dec = decorations[result]
                loc = int(dec.get('Location', ['-1'])[0])
                memory[result] = [[bits(1000+loc*100+v*10+c) for c in range(4)] for v in range(int(path.name[0])+1)]
            elif args[1] == 'Output':
                outputs.append(result)
                target = types[types[args[0]][1][1]]
                memory[result] = [None]*int(target[1][1]) if target[0] == 'OpTypeVector' else None
        elif op == 'OpFunction':
            in_function = True
        elif op == 'OpAccessChain':
            base, indices = pointers[args[1]]
            pointers[result] = base, indices + [values[x] for x in args[2:]]
        elif op == 'OpLoad':
            base, indices = pointers[args[1]]
            value = memory[base]
            for i in indices:
                value = value[i]
            values[result] = copy.deepcopy(value)
        elif op == 'OpCompositeExtract':
            values[result] = values[args[1]][int(args[2])]
        elif op == 'OpCompositeInsert':
            values[result] = copy.deepcopy(values[args[2]])
            values[result][int(args[3])] = values[args[1]]
        elif op in ('OpFAdd', 'OpFMul'):
            a, b = unbits(values[args[1]]), unbits(values[args[2]])
            values[result] = bits(a+b if op == 'OpFAdd' else a*b)
        elif op == 'OpBitcast':
            values[result] = values[args[1]]  # All values are stored as raw 32-bit bits.
        elif op == 'OpStore':
            base, indices = pointers[args[0]]
            # Negative control: omit restores immediately after emission.
            if drop_restore and after_emit:
                continue
            if indices:
                target = memory[base]
                for i in indices[:-1]:
                    target = target[i]
                target[indices[-1]] = copy.deepcopy(values[args[1]])
            else:
                memory[base] = copy.deepcopy(values[args[1]])
        elif op == 'OpEmitVertex':
            emitted.append({x: copy.deepcopy(memory[x]) for x in outputs})
            if poison:
                for x in outputs:
                    memory[x] = [None]*len(memory[x]) if isinstance(memory[x], list) else None
            after_emit = True
        elif op == 'OpEndPrimitive':
            ended += 1
        elif op in ('OpLabel', 'OpReturn', 'OpFunctionEnd'):
            pass
        elif in_function:
            raise AssertionError(f'Unhandled instruction {op}')
        # Restore region ends when the next input load/access begins.
        if op == 'OpAccessChain':
            base, _ = pointers[result]
            if base not in outputs:
                after_emit = False
    return memory, decorations, outputs, passed, emitted, ended


def check(directory):
    for topology in range(3):
        for mask in range(3):
            native = pathlib.Path(directory)/f'{topology}-{mask}-0.spvasm'
            mem, dec, out, passed, _, _ = run(native)
            reference = []
            for vertex in range(topology+1):
                reference.append({**{tuple(dec[x].get('BuiltIn', dec[x].get('Location'))): mem[x] for x in out},
                    **{tuple(dec[x].get('BuiltIn', dec[x].get('Location'))): mem[x][vertex] for x in passed}})
            for mode in (1, 2):
                path = pathlib.Path(directory)/f'{topology}-{mask}-{mode}.spvasm'
                _, dec, out, passed, emitted, ended = run(path)
                assert not passed
                assert len(emitted) == topology+1 and ended == 1
                for v, actual in enumerate(emitted):
                    actual = {tuple(dec[x].get('BuiltIn', dec[x].get('Location'))): value for x, value in actual.items()}
                    assert actual == reference[v], (path, v, actual, reference[v])
                text = path.read_text()
                assert 'GeometryShaderPassthroughNV' not in text and 'SPV_NV_geometry_shader_passthrough' not in text
            assert 'GeometryShaderPassthroughNV' in native.read_text()
    for topology in range(3):
        for mode in (1, 2):
            path = pathlib.Path(directory)/f'{topology}-3-{mode}.spvasm'
            _, dec, _, _, emitted, ended = run(path)
            assert len(emitted) == topology+1 and ended == 1
            for vertex, actual in enumerate(emitted):
                actual = {tuple(dec[x].get('BuiltIn', dec[x].get('Location'))): v for x, v in actual.items()}
                assert actual[('Layer',)] == 7 and actual[('ViewportIndex',)] == 2
                assert actual[('0',)] == [bits(1000+vertex*10), bits(17), bits(1002+vertex*10), bits(19)]
                assert actual[('Position',)] == [bits(900+vertex*10), bits(0.5), bits((902+vertex*10+1)*0.5), bits(1)]
    # This must catch the old loss of Layer/ViewportIndex/generic primitive outputs.
    path = pathlib.Path(directory)/'2-2-1.spvasm'
    _, dec, _, _, emitted, _ = run(path, drop_restore=True)
    layer = next(x for x in dec if dec[x].get('BuiltIn') == ['Layer'] and x in emitted[0])
    assert emitted[0][layer] == 7 and emitted[1][layer] is None
    print('PASS native vs explicit: 24 explicit modules, sparse masks/depth, vertex order/count, masks, position, layer, viewport, generic0/1/31; negative control catches lost outputs')


if __name__ == '__main__':
    check(sys.argv[1])

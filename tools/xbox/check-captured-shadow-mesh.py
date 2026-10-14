# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Forward-check the captured skinned shadow mesh against native D16.

Specific captured float3/UNORM8x2/UINT8x2 layout, verified before use. CPU
rasterization omits clipped triangles and edges. Later geometry can occlude
predictions; a nearer observed value is not a mismatch in this draw.
"""
import argparse
import collections
import importlib.util
import json
import math
from pathlib import Path
import re
import struct

spec = importlib.util.spec_from_file_location('lighting', Path(__file__).with_name('replay-lighting-ir.py'))
lighting = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lighting)


def check(root, ordinal, original, draws):
    prefix = sorted((root / 'trace').glob('*_probe.csv'))[ordinal].stem[:-6]
    draw = next(d for d in draws if d['vs'] == original['hash'] and d['ps'] == '97d814a92d02f725')
    state = draw['state']
    fetches = []
    for attr, dxgi in ((0, 6), (2, 49), (3, 50)):
        fetch = re.search(r'fetch' + str(attr) + r'\[dxgi=' + str(dxgi) +
                          r' fallback=false offset=(\d+) stride=(\d+) divisor=0 parts=1', state)
        if not fetch:
            raise ValueError('Unsupported attribute layout')
        fetches.append(tuple(map(int, fetch.groups())))
    stride = fetches[0][1]
    if any(f[1] != stride for f in fetches) or fetches[0][0] + 12 > stride or any(
            f[0] + 2 > stride for f in fetches[1:]):
        raise ValueError('Invalid vertex stream layout')
    for fragment in ('fb 1024x1024', 'slope=0', 'scale 0.5 translate 0.5 -> D3D 0..1 flip 0'):
        if fragment not in state:
            raise ValueError('Unsupported shadow mesh state: ' + fragment)
    stem = prefix + f"_draw_{draw['index']}_vs_{original['hash']}_ps_97d814a92d02f725"
    files = list((root / 'trace').glob(stem + '*kind_2_slot_*.bin'))
    files.sort(key=lambda p: int(re.search(r'slot_(\d+)', p.name)[1]))
    cb = {d['index']: p.read_bytes() for d, p in zip(original['cbuf_descriptors'], files)}
    if len(files) != 5 or struct.unpack_from('<f', files[-1].read_bytes(), 20)[0] != 1:
        raise ValueError('Shadow pixel demote/dither controls not proven inactive')
    geometry = next((root / 'trace').glob(stem + '*kind_3_slot_0.bin')).read_bytes()
    indices = next((root / 'trace').glob(stem + '*kind_4_slot_0.bin')).read_bytes()
    count = int(re.search(r'draw indexed \((\d+) vertices', state)[1])
    if len(geometry) % stride or len(indices) != count * 2 or count % 3:
        raise ValueError('Incomplete mesh')
    vertex = lighting.ir.attribute_slice(original, {28, 29, 30, 31})
    points = []
    for start in range(0, len(geometry), stride):
        pos = struct.unpack_from('<3f', geometry, start + fetches[0][0])
        attributes = dict(zip((32, 33, 34), pos))
        weights, bones = start + fetches[1][0], start + fetches[2][0]
        attributes.update({40: lighting.ir.f32(geometry[weights] / 255),
            41: lighting.ir.f32(geometry[weights + 1] / 255),
            44: lighting.ir.bits_float(geometry[bones]),
            45: lighting.ir.bits_float(geometry[bones + 1])})
        output = lighting.ir.evaluate(vertex, cb, attributes)[0]
        x, y, z, w = (output[a] for a in range(28, 32))
        points.append((512 * (1 + x / w), 512 * (1 - y / w), .5 * (z / w + 1), w))
    triangles = list(struct.iter_unpack('<3H', indices))
    depth = next((root / 'trace').glob(prefix + '*sampled_depth_523490000_m0_l0.raw')).read_bytes()
    if len(depth) != 1024 * 1024 * 2:
        raise ValueError('Incomplete shadow surface')
    host_bias = float(re.search(r'host=(-?[\d.]+)', state)[1])
    results = []
    for winding in (-1, 1):
        predicted = {}
        for triangle in triangles:
            p = [points[i] for i in triangle]
            if any(v[3] <= 0 or not 0 < v[2] < 1 for v in p):
                continue
            a, b, c = p
            area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if area * winding <= 0:
                continue
            for y in range(max(0, math.floor(min(v[1] for v in p))), min(1024, math.ceil(max(v[1] for v in p)))):
                for x in range(max(0, math.floor(min(v[0] for v in p))), min(1024, math.ceil(max(v[0] for v in p)))):
                    px, py = x + .5, y + .5
                    wa = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area
                    wb = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area
                    wc = 1 - wa - wb
                    if min(wa, wb, wc) <= .02:
                        continue
                    z = wa * a[2] + wb * b[2] + wc * c[2]
                    predicted[x, y] = min(predicted.get((x, y), 1), z)
        for bias in (host_bias - 100, host_bias, host_bias + 100):
            errors = []
            for (x, y), z in predicted.items():
                expected = min(65535, max(0, round(z * 65535 + bias)))
                observed = struct.unpack_from('<H', depth, 2 * (y * 1024 + x))[0]
                errors.append(observed - expected)
            results.append(dict(winding=winding, bias=bias, samples=len(errors),
                within_two=sum(abs(e) <= 2 for e in errors),
                nearer_observed_depth=sum(e < -2 for e in errors),
                missing_predicted_depth=sum(e > 2 for e in errors),
                error_histogram=dict(collections.Counter(errors))))
    return dict(capture=ordinal + 1, draw=draw['index'], vertices=len(points), triangles=len(triangles), results=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('vertex_ir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = json.loads(args.vertex_ir.read_text(encoding='utf-8'))
    captures = lighting.material.captures((args.capture_root / 'eden_log.txt').read_text(
        encoding='utf-8', errors='replace').splitlines())
    results = [check(args.capture_root, i, original, c['draws']) for i, c in enumerate(captures)]
    args.output.write_text(json.dumps(results, indent=2), encoding='utf-8')
    for result in results:
        print(result['capture'], result['results'][1])


if __name__ == '__main__':
    main()

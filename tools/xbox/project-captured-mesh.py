# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project captured geometry using its actual vertex IR position slice.

Supports one complete float3 position vertex stream and uint16 triangle indices.
Exports a wireframe SVG and bounds to locate an object; does not establish pixel
shader coverage, depth-test success or transparency correctness.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct

spec = importlib.util.spec_from_file_location('lighting', Path(__file__).with_name('replay-lighting-ir.py'))
lighting = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lighting)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('vertex_ir', type=Path)
    parser.add_argument('pixel_shader')
    parser.add_argument('--capture', type=int, required=True, help='One-based capture ordinal')
    parser.add_argument('--draw', type=int, help='Exact draw index; otherwise the first shader pair')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = json.loads(args.vertex_ir.read_text(encoding='utf-8'))
    vertex = lighting.ir.attribute_slice(original, {28, 29, 30, 31})
    log = (args.capture_root / 'eden_log.txt').read_text(encoding='utf-8', errors='replace')
    captures = lighting.material.captures(log.splitlines())
    matches = [d for d in captures[args.capture - 1]['draws']
               if d['vs'] == vertex['hash'] and d['ps'] == args.pixel_shader and
               (args.draw is None or d['index'] == args.draw)]
    if not matches:
        raise ValueError('Requested draw/shader pair is absent from the capture')
    draw = matches[0]
    prefix = sorted((args.capture_root / 'trace').glob('*_probe.csv'))[args.capture - 1].stem[:-6]
    stem = f"{prefix}_draw_{draw['index']}_vs_{vertex['hash']}_ps_{args.pixel_shader}"
    files = list((args.capture_root / 'trace').glob(stem + '*kind_2_slot_*.bin'))
    files.sort(key=lambda p: int(re.search(r'slot_(\d+)', p.name)[1]))
    cb = {d['index']: p.read_bytes() for d, p in zip(vertex['cbuf_descriptors'], files)}
    geometry = next((args.capture_root / 'trace').glob(stem + '*kind_3_slot_0.bin')).read_bytes()
    indices = next((args.capture_root / 'trace').glob(stem + '*kind_4_slot_0.bin')).read_bytes()
    fetch = re.search(r'fetch0\[dxgi=(\d+) fallback=false offset=(\d+) stride=(\d+)', draw['state'])
    if not fetch or int(fetch[1]) != 6:
        raise ValueError('Requires float3 position')
    offset, stride = int(fetch[2]), int(fetch[3])
    count = int(re.search(r'draw indexed \((\d+) vertices', draw['state'])[1])
    if len(indices) != count * 2 or len(geometry) % stride or count % 3:
        raise ValueError('Incomplete or unsupported geometry')
    # Match the first-input log record by pair, draw and count. Base/first offsets
    # are not assumed from the filename. Current complete stream case starts at zero.
    params = re.search(r'D3D12 material inputs: draw=' + str(draw['index']) + r' vs_' + vertex['hash'] +
        r'_ps_' + args.pixel_shader + r' vertices=' + str(count) +
        r' instances=(\d+) first_index=(\d+) base_vertex=(-?\d+) base_instance=(\d+) indexed=(\w+)', log)
    if not params or tuple(map(int, params.groups()[:4])) != (1, 0, 0, 0):
        raise ValueError('Unsupported instancing/base/first index')
    points = []
    for start in range(0, len(geometry), stride):
        pos = struct.unpack_from('<3f', geometry, start + offset)
        output = lighting.ir.evaluate(vertex, cb, dict(zip((32, 33, 34), pos)))[0]
        x, y, z, w = (output[a] for a in range(28, 32))
        points.append((960 * (1 + x / w), 540 * (1 - y / w), .5 * (z / w + 1), w))
    triangles = list(struct.iter_unpack('<3H', indices))
    if max(v for t in triangles for v in t) >= len(points):
        raise ValueError('Indices exceed captured vertex stream')
    used = [points[v] for v in sorted({v for t in triangles for v in t})]
    report = dict(draw=draw['index'], vertex_shader=vertex['hash'], pixel_shader=args.pixel_shader,
        vertices=len(points), triangles=len(triangles),
        screen_bounds=[min(p[c] for p in used) for c in range(2)] + [max(p[c] for p in used) for c in range(2)],
        depth_bounds=[min(p[2] for p in used), max(p[2] for p in used)],
        vertices_behind_eye=sum(p[3] <= 0 for p in used), points=points, indices=triangles)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    paths = []
    for tri in triangles:
        p = [points[i] for i in tri]
        if any(v[3] <= 0 for v in p):
            continue
        paths.append('M' + ' L'.join(f'{v[0]:.3f},{v[1]:.3f}' for v in p) + ' Z')
    args.output.with_suffix('.svg').write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="1920" height="1080" viewBox="0 0 1920 1080">'
        '<rect width="1920" height="1080" fill="#202020"/>'
        '<path fill="none" stroke="#40ff90" stroke-width="0.5" d="' + ' '.join(paths) + '"/></svg>', encoding='utf-8')
    print({k: v for k, v in report.items() if k not in ('points', 'indices')})


if __name__ == '__main__':
    main()

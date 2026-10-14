# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check captured depth reconstruction, shadow matrices and G-buffer pixel history.

Requires typed IR from inspect-captured-shader. These are internal consistency
checks, not an independent reference rendering. No changes to capture evidence.
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


def matmul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def matrix(data, offset, rows):
    result = [list(struct.unpack_from('<4f', data, offset + 16 * r)) for r in range(rows)]
    if rows == 3:
        result.append([0, 0, 0, 1])
    return result


def analyze(capture):
    result = {'draw': capture.draw['index'], 'prefix': capture.prefix}
    depth_paths = list((capture.root / 'trace').glob(capture.prefix + '*sampled_depth_5362a0000_m0_l0.raw'))
    if len(depth_paths) != 1:
        raise ValueError('Expected one scene-depth source, not a final depth substitute')
    scene = depth_paths[0].read_bytes()
    info = json.loads(depth_paths[0].with_suffix('.json').read_text(encoding='utf-8'))
    if info['dxgi_format'] != 39 or (info['width'], info['height']) != (capture.width, capture.height):
        raise ValueError('Unsupported scene depth layout')
    cb4 = capture.pcb[4]
    near, far = struct.unpack_from('<f', cb4, 332)[0], struct.unpack_from('<f', cb4, 336)[0]
    if not 0 < near < far:
        raise ValueError('Invalid camera near/far')
    tile = capture.textures[3]['tile']
    stats = dict(samples=0, rne_exact=0, rtz_exact=0, max_half_ulp=0)
    coordinates_exact = 0
    history = []
    for y in range(tile['y'] + 2, tile['y'] + tile['height'] - 2, 4):
        for x in range(tile['x'] + 2, tile['x'] + tile['width'] - 2, 4):
            depth = struct.unpack_from('<f', scene, 4 * (y * capture.width + x))[0]
            distance = far * near / (far - depth * (far - near))
            linear = (distance - near) / (far - near)
            nearest = struct.unpack('<H', struct.pack('<e', linear))[0]
            truncated = nearest - (struct.unpack('<e', struct.pack('<H', nearest))[0] > linear)
            stored = struct.unpack_from('<H', tile['data'], 4 * ((y - tile['y']) * tile['width'] + x - tile['x']))[0]
            stats['samples'] += 1
            stats['rne_exact'] += nearest == stored
            stats['rtz_exact'] += truncated == stored
            stats['max_half_ulp'] = max(stats['max_half_ulp'], abs(nearest - stored))
            uv = ((x + .5) / capture.width, (y + .5) / capture.height)
            ld = capture.sample(3, uv, None, None, {})[0]
            normal = capture.sample(2, uv, None, None, {})
            predicted = capture.shadow_coordinates(x, y, ld, normal)
            taps = capture.pixel_at(x, y, rounding='rtz')['taps']
            for p, t in zip(predicted, taps):
                actual = (*t['uv'], t['reference'])
                if tuple(map(lighting.ir.float_bits, p)) != tuple(map(lighting.ir.float_bits, actual)):
                    raise ValueError('Factored shadow expression disagrees with typed IR')
                coordinates_exact += 1
    result['depth_reconstruction'] = stats
    result['factored_shadow_taps_bit_exact'] = coordinates_exact

    # Captured first shadow mesh's producer matrix versus lighting's consumer.
    shadow_files = sorted((capture.root / 'trace').glob(capture.prefix + '*_vs_886e2c7876ffb85b_ps_97d814a92d02f725*kind_2_slot_0.bin'))
    if shadow_files:
        shadow = matrix(shadow_files[0].read_bytes(), 96, 4)
        light_world = matmul(matrix(capture.pcb[13], 0, 4), matrix(cb4, 0, 3))
        expected = [[.5 * (shadow[0][c] + shadow[3][c]) for c in range(4)],
                    [.5 * (-shadow[1][c] + shadow[3][c]) for c in range(4)],
                    [.5 * (shadow[2][c] + shadow[3][c]) for c in range(4)], shadow[3]]
        result['shadow_matrix_max_abs_error'] = max(abs(light_world[r][c] - expected[r][c])
                                                   for r in range(4) for c in range(4))
        result['shadow_matrix_expected_max_abs'] = max(abs(v) for row in expected for v in row)
    else:
        result['shadow_matrix_missing'] = 'First shadow mesh inputs not captured'
    view_inverse = matmul(matrix(cb4, 0, 3), matrix(cb4, 48, 3))
    result['view_inverse_max_abs_error'] = max(abs(view_inverse[r][c] - (r == c))
                                              for r in range(4) for c in range(4))
    # Track byte changes; unchanged writes and intervening clears cannot be attributed.
    x, y = capture.width // 2, capture.height // 2
    for texture in capture.textures[:4]:
        previous = None
        changes = []
        for t in capture.tiles:
            if t['address'] != texture['address'] or t['draw'] >= capture.draw['index']:
                continue
            if not (t['x'] <= x < t['x'] + t['width'] and t['y'] <= y < t['y'] + t['height']):
                continue
            offset = 4 * ((y - t['y']) * t['width'] + x - t['x'])
            value = t['data'][offset:offset + 4]
            if value != previous:
                changes.append(dict(draw=t['draw'], raw=value.hex(),
                                    rgba=lighting.probe.texel(value, t['dxgi'])))
                previous = value
        history.append(dict(address=texture['address'], dxgi=texture['dxgi'], changes=changes))
    result['center_pixel_history'] = history
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('ir_directory', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    vertex = json.loads((args.ir_directory / 'd4896447533c2378.ir.json').read_text(encoding='utf-8'))
    pixel = json.loads((args.ir_directory / 'd3e840710932bff7.ir.json').read_text(encoding='utf-8'))
    captures = lighting.material.captures((args.capture_root / 'eden_log.txt').read_text(
        encoding='utf-8', errors='replace').splitlines())
    results = [analyze(lighting.Capture(args.capture_root, i, vertex, pixel)) for i in range(len(captures))]
    args.output.write_text(json.dumps(results, indent=2), encoding='utf-8')
    for i, result in enumerate(results):
        print(i + 1, result['depth_reconstruction'],
              'matrix_error', result.get('shadow_matrix_max_abs_error', 'missing'))


if __name__ == '__main__':
    main()

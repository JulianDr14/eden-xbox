# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Re-evaluate a straight-line lighting IR on pre-draw native ROI inputs.

Consumes captured data only. Does not modify resources, shaders or rendering.
Software filtering/reciprocal and interpolator rounding are approximations;
quantized output comparisons and one-variable controls expose their limits.
"""
import argparse
import csv
import importlib.util
import json
import math
from pathlib import Path
import re
import struct
import collections


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


ir = module('scalar_ir', 'ir-scalar-evaluator.py')
probe = module('probe', 'analyze-material-probe.py')
material = module('material', 'analyze-material-trace.py')


def encode_small(value, mantissa, rounding='rne'):
    convert = round if rounding == 'rne' else math.floor
    if value <= 0: return 0
    if not math.isfinite(value): return 31 << mantissa
    exponent = math.floor(math.log2(value))
    if exponent < -14:
        return min(1 << mantissa, convert(math.ldexp(value, 14 + mantissa)))
    if exponent > 15: return 31 << mantissa
    significand = convert(math.ldexp(value, mantissa - exponent))
    if significand == 2 << mantissa:
        exponent += 1
        significand >>= 1
    return ((exponent + 15) << mantissa) | (significand - (1 << mantissa))


def pack_r11(values, rounding='rne'):
    return encode_small(values[0], 6, rounding) | (encode_small(values[1], 6, rounding) << 11) | \
           (encode_small(values[2], 5, rounding) << 22)


def native_input_tile(path, address, sampling_format):
    """Load tightly packed, exact pre-draw native bytes, never a clamped preview."""
    info = json.loads(path.with_suffix('.json').read_text(encoding='utf-8'))
    compatible = {28: (27, 28), 26: (26,), 24: (23, 24), 34: (33, 34)}
    if (info.get('native_bytes') is not True or info.get('origin') != 'top-left' or
            info['dxgi_format'] not in compatible.get(sampling_format, ()) or
            info['row_bytes'] != info['width'] * 4):
        raise ValueError('Unsupported native lighting input layout: ' + str(path))
    data = path.read_bytes()
    if len(data) != info['row_bytes'] * info['height']:
        raise ValueError('Incomplete native lighting input: ' + str(path))
    return dict(x=0, y=0, width=info['width'], height=info['height'], data=data,
                address=address, dxgi=sampling_format, subresource=0)


class Capture:
    def __init__(self, root, capture_index, vertex, pixel):
        self.root, self.vertex, self.pixel = root, vertex, pixel
        captures = material.captures((root / 'eden_log.txt').read_text(
            encoding='utf-8', errors='replace').splitlines())
        self.draw = next(d for d in captures[capture_index]['draws'] if d['ps'] == pixel['hash'])
        self.prefix = sorted((root / 'trace').glob('*_probe.csv'))[capture_index].stem[:-6]
        self.tiles = probe.load_tiles(root / 'trace' / (self.prefix + '_probe.csv'))
        self.bindings = []
        for binding in self.draw['bindings'].split(' / '):
            found = re.search(r'cbuf=(\d+)\+([0-9a-f]+).*?srv=\[@([0-9a-f]+).*?'
                              r'dxgi=\d+/(\d+).*?size=(\d+)x(\d+).*?mapping=([0-9a-f]+)', binding)
            if found:
                self.bindings.append(dict(cbuf=int(found[1]), offset=int(found[2], 16),
                    address=found[3], dxgi=int(found[4]), width=int(found[5]),
                    height=int(found[6]), mapping=int(found[7], 16)))
        self.textures = []
        for descriptor in pixel['textures']:
            binding = next(b for b in self.bindings if b['cbuf'] == descriptor['cbuf']
                           and b['offset'] == descriptor['offset'])
            if descriptor['depth']:
                path = next((root / 'trace').glob(self.prefix + '*sampled_depth_' +
                                                binding['address'] + '_m0_l0.raw'))
                binding = dict(binding, data=path.read_bytes())
                if binding['dxgi'] != 56 or len(binding['data']) != binding['width'] * binding['height'] * 2:
                    raise ValueError('Incomplete or unsupported sampled D16 surface')
            else:
                full = list((root / 'trace').glob(self.prefix + '_before_draw_' +
                    str(self.draw['index']) + '_ps_' + pixel['hash'] + '_input_' +
                    binding['address'] + '_m0_l0.raw'))
                if len(full) > 1: raise ValueError('Ambiguous native pre-draw input')
                if full:
                    tile = native_input_tile(full[0], binding['address'], binding['dxgi'])
                    if (tile['width'], tile['height']) != (binding['width'], binding['height']):
                        raise ValueError('Native input does not cover the bound view')
                    self.textures.append(dict(binding, tile=tile))
                    continue
                tiles = [t for t in self.tiles if t['address'] == binding['address'] and
                         t['draw'] < self.draw['index'] and t.get('subresource', 0) == 0]
                if not tiles: raise ValueError('Missing pre-draw input ' + str(binding))
                binding = dict(binding, tile=tiles[-1])
            self.textures.append(binding)
        files = list((root / 'trace').glob(self.prefix + '*_ps_' + pixel['hash'] + '*kind_2_slot_*.bin'))
        files.sort(key=lambda p: int(re.search(r'slot_(\d+)', p.name)[1]))
        descriptors = vertex['cbuf_descriptors'] + pixel['cbuf_descriptors']
        if len(files) != len(descriptors): raise ValueError('Uniform descriptor count mismatch')
        split = len(vertex['cbuf_descriptors'])
        self.vcb = {d['index']: p.read_bytes() for d, p in zip(descriptors[:split], files[:split])}
        self.pcb = {d['index']: p.read_bytes() for d, p in zip(descriptors[split:], files[split:])}
        target = next((root / 'trace').glob(self.prefix + '*after_draw_*_ps_' + pixel['hash'] + '_rt0.raw'), None)
        self.target_tile = None
        if target:
            self.actual = target.read_bytes()
            self.target_info = json.loads(target.with_suffix('.json').read_text(encoding='utf-8'))
        else:
            # The exact post-draw ROI remains usable when a map-specific PS did
            # not match the configured full-target snapshot selector.
            matches = [t for t in self.tiles if t['draw'] == self.draw['index'] and t['target'] == 0]
            if len(matches) != 1: raise ValueError('Missing unambiguous post-draw output')
            self.target_tile = matches[0]
            extent = re.search(r'\| fb (\d+)x(\d+)', self.draw['state'])
            if not extent: raise ValueError('Missing framebuffer extent')
            self.actual = None
            self.target_info = dict(width=int(extent[1]), height=int(extent[2]),
                                    dxgi_format=self.target_tile['dxgi'])
        if self.target_info['dxgi_format'] != 26: raise ValueError('Expected R11G11B10 output')
        self.width, self.height = self.target_info['width'], self.target_info['height']
        if self.actual is not None and len(self.actual) != self.width * self.height * 4:
            raise ValueError('Incomplete full post-draw output')
        self.corners = [ir.evaluate(vertex, self.vcb, {32: x, 33: y, 34: 1.0})[0]
                        for x, y in ((-1, 1), (1, 1), (-1, -1))]

    def attributes(self, x, y, flip_varying=False):
        u, v = ir.f32((x + 0.5) / self.width), ir.f32((y + 0.5) / self.height)
        if flip_varying: v = ir.f32(1 - v)
        tl, tr, bl = self.corners
        return {key: ir.f32(tl[key] + (tr[key] - tl[key]) * u + (bl[key] - tl[key]) * v)
                for key in (32, 33, 36, 37)}

    def sample(self, index, uv, reference, inst, controls):
        texture = self.textures[index]
        if index == 1 and controls.get('zero_lighting_input'):
            if texture['dxgi'] != 26:
                raise ValueError('Expected captured R11 lighting input for this control')
            return (0.0, 0.0, 0.0, 1.0)
        if reference is not None:
            return ir.compare_depth_bilinear(texture['data'], texture['width'], texture['height'],
                uv, reference, flip_y=controls.get('flip_shadow', False),
                reverse=controls.get('reverse_compare', False),
                fraction_bits=controls.get('fraction_bits', 8), force=controls.get('force_shadow'))
        tile = texture['tile']
        x, y = uv[0] * texture['width'] - 0.5, uv[1] * texture['height'] - 0.5
        ix, iy = math.floor(x), math.floor(y)
        fx, fy = round((x - ix) * 256) / 256, round((y - iy) * 256) / 256
        result = [0.0] * 4
        for dx, wx in ((0, 1 - fx), (1, fx)):
            for dy, wy in ((0, 1 - fy), (1, fy)):
                if wx * wy == 0: continue
                px, py = ix + dx - tile['x'], iy + dy - tile['y']
                if not (0 <= px < tile['width'] and 0 <= py < tile['height']):
                    raise ValueError('Sample outside captured input tile')
                at = (py * tile['width'] + px) * 4
                rgba = probe.texel(tile['data'][at:at + 4], texture['dxgi'])
                rgba = tuple((rgba + (0, 1))[(texture['mapping'] >> (3 * c)) & 7] for c in range(4))
                for c in range(4): result[c] += rgba[c] * wx * wy
        if index == 0 and controls.get('zero_albedo_alpha'):
            if texture['dxgi'] != 28:
                raise ValueError('Expected RGBA8 color input for alpha control')
            result[3] = 0.0
        return tuple(ir.f32(v) for v in result)

    def pixel_at(self, x, y, **controls):
        taps = []
        def sample(index, uv, reference, inst):
            value = self.sample(index, uv, reference, inst, controls)
            if reference is not None:
                taps.append({'uv': uv, 'reference': reference, 'result': value, 'inst': inst['id']})
            return value
        color, values, trace = ir.evaluate(self.pixel, self.pcb,
            self.attributes(x, y, controls.get('flip_varying', False)), sample,
            fused=not controls.get('split_fma', False))
        rgba = tuple(color[0, c] for c in range(4))
        if self.actual is not None:
            actual = struct.unpack_from('<I', self.actual, (y * self.width + x) * 4)[0]
        else:
            tile = self.target_tile
            if not (tile['x'] <= x < tile['x'] + tile['width'] and tile['y'] <= y < tile['y'] + tile['height']):
                raise ValueError('Output pixel outside exact post-draw tile')
            actual = struct.unpack_from('<I', tile['data'], ((y - tile['y']) * tile['width'] + x - tile['x']) * 4)[0]
        return dict(x=x, y=y, rgba=rgba, packed=pack_r11(rgba, controls.get('rounding', 'rne')), actual=actual,
                    actual_rgba=probe.texel(struct.pack('<I', actual), 26), taps=taps)

    def shadow_coordinates(self, x, y, linear_depth, normal):
        """Factored expression of this captured shader; validated against every IR tap.

        Explicitly require its descriptor pattern instead of silently accepting other shaders.
        """
        if [t['dxgi'] for t in self.textures] != [28, 26, 24, 34, 56]:
            raise ValueError('Unsupported lighting resource pattern')
        cb4 = struct.unpack('<' + 'f' * (len(self.pcb[4]) // 4), self.pcb[4])
        cb13 = struct.unpack('<' + 'f' * (len(self.pcb[13]) // 4), self.pcb[13])
        epsilon = struct.unpack_from('<f', self.pcb[12], 28)[0]
        attr = self.attributes(x, y)
        z = ir._fma(linear_depth, cb4[85], cb4[83])
        p = (ir._fma(ir._fma(normal[0], 2, -1), cb13[68], ir.f32(-attr[36] * z)),
             ir._fma(ir._fma(normal[1], 2, -1), cb13[68], ir.f32(-attr[37] * z)),
             ir._fma(ir._fma(normal[2], 2, -1), cb13[68], -z))
        clip = []
        for row in range(4):
            m = cb13[row * 4:row * 4 + 4]
            clip.append(ir.f32(ir._fma(p[2], m[2], ir._fma(p[1], m[1],
                                ir.f32(p[0] * m[0]))) + m[3]))
        reciprocal = ir.f32(1 / clip[3])
        reference = ir.f32(clip[2] * reciprocal)
        return [(ir._fma(clip[0], reciprocal, sx * epsilon),
                 ir._fma(clip[1], reciprocal, sy * epsilon), reference)
                for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_root', type=Path)
    parser.add_argument('vertex_ir', type=Path)
    parser.add_argument('pixel_ir', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--step', type=int, default=8)
    parser.add_argument('--rounding', choices=('rne', 'rtz'), default='rne')
    parser.add_argument('--experiments', default='all')
    parser.add_argument('--region', type=int, nargs=4, metavar=('X', 'Y', 'WIDTH', 'HEIGHT'),
                        help='Native pixel region; full pre-draw inputs allow sampling outside the ROI')
    parser.add_argument('--require-full-inputs', action='store_true',
                        help='Reject ROI-only inputs or output; verify the selected full-pass capture')
    parser.add_argument('--validate-only', action='store_true', help='Report coverage without replaying pixels')
    args = parser.parse_args()
    if args.step <= 0: parser.error('--step must be positive')
    vertex = json.loads(args.vertex_ir.read_text(encoding='utf-8'))
    pixel = json.loads(args.pixel_ir.read_text(encoding='utf-8'))
    all_results = []
    count = len(material.captures((args.capture_root / 'eden_log.txt').read_text(
        encoding='utf-8', errors='replace').splitlines()))
    for index in range(count):
        capture = Capture(args.capture_root, index, vertex, pixel)
        coverage = [dict(address=t['address'], dxgi=t['dxgi'],
                         full=t['tile']['x'] == 0 and t['tile']['y'] == 0 and
                              t['tile']['width'] == capture.width and t['tile']['height'] == capture.height)
                    for t in capture.textures if 'tile' in t]
        if args.require_full_inputs and (not all(t['full'] for t in coverage) or capture.actual is None):
            raise ValueError('Missing full pre-draw inputs or exact full post-draw output')
        if args.validate_only:
            all_results.append(dict(capture=index + 1, draw=capture.draw['index'], inputs=coverage,
                                    full_output=capture.actual is not None))
            print(index + 1, 'coverage', coverage, 'full_output', capture.actual is not None)
            continue
        tile = capture.textures[0]['tile']
        x, y, width, height = args.region or (tile['x'] + 2, tile['y'] + 2,
                                             tile['width'] - 4, tile['height'] - 4)
        if width <= 0 or height <= 0 or x < 0 or y < 0 or x + width > capture.width or y + height > capture.height:
            raise ValueError('Invalid native pixel region')
        points = [(px, py) for py in range(y, y + height, args.step)
                  for px in range(x, x + width, args.step)]
        experiments = []
        for name, controls in [('baseline', {}), ('split_fma', {'split_fma': True}),
                ('lighting_input_zero', {'zero_lighting_input': True}),
                ('albedo_alpha_zero', {'zero_albedo_alpha': True}),
                ('flip_shadow_y', {'flip_shadow': True}), ('reverse_compare', {'reverse_compare': True}),
                ('shadow_all_lit', {'force_shadow': 1.0}), ('shadow_all_dark', {'force_shadow': 0.0}),
                ('flip_varying_y', {'flip_varying': True}), ('filter_unquantized', {'fraction_bits': None})]:
            if args.experiments != 'all' and name not in args.experiments.split(','): continue
            controls['rounding'] = args.rounding
            samples = [capture.pixel_at(x, y, **controls) for x, y in points]
            exact = sum(s['packed'] == s['actual'] for s in samples)
            error = sum(sum(abs(a - b) for a, b in zip(s['rgba'][:3], s['actual_rgba'][:3]))
                        for s in samples) / (3 * len(samples))
            experiments.append({'name': name, 'samples': len(samples), 'exact': exact,
                                'mean_abs_rgb_error': error, 'pixels': samples})
            print(index + 1, name, 'exact', exact, '/', len(samples), 'MAE', round(error, 6), flush=True)
        all_results.append({'capture': index + 1, 'draw': capture.draw['index'],
                            'experiments': experiments})
    args.output.write_text(json.dumps(all_results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()

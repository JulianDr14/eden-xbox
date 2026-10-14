#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare isolated GPU intermediate ROI exports with native captured inputs.

This probe's fixed interface is documented in xbox_sm3dw_lighting.md. Mask probes
use screenUV at explicit LOD 0/9, not the original shader's projected UVs.
"""
import argparse
import json
import math
import re
import struct
from pathlib import Path


def native(path, formats, pixel_bytes):
    meta = json.loads(path.with_suffix('.json').read_text(encoding='utf8'))
    width, height = meta['width'], meta['height']
    if not meta.get('native_bytes') or meta.get('dxgi_format') not in formats:
        raise ValueError(f'unsupported native metadata: {path}')
    if not isinstance(width, int) or not isinstance(height, int) or not (0 < width <= 8192 and 0 < height <= 8192):
        raise ValueError('invalid dimensions')
    data = path.read_bytes()
    if len(data) != width * height * pixel_bytes or meta['row_bytes'] != width * pixel_bytes:
        raise ValueError(f'incomplete native data: {path}')
    return width, height, data


class BC4:
    def __init__(self, path):
        meta = json.loads(path.with_suffix('.json').read_text(encoding='utf8'))
        self.w, self.h = meta['width'], meta['height']
        if meta.get('dxgi_format') not in (79, 80) or not meta.get('native_bytes'):
            raise ValueError('mask must be native BC4_UNORM')
        if not (0 < self.w <= 8192 and 0 < self.h <= 8192):
            raise ValueError('invalid BC4 dimensions')
        self.data = path.read_bytes()
        self.row = (self.w + 3) // 4 * 8
        if len(self.data) != self.row * ((self.h + 3) // 4):
            raise ValueError('incomplete BC4 mip')
        self.blocks = {}

    def pixel(self, x, y):
        x, y = x % self.w, y % self.h
        offset = y // 4 * self.row + x // 4 * 8
        if offset not in self.blocks:
            a, b = self.data[offset:offset+2]
            values = [a / 255, b / 255]
            values += ([(a * (7-i) + b * i) / (7*255) for i in range(1, 7)] if a > b else
                       [(a * (5-i) + b * i) / (5*255) for i in range(1, 5)] + [0, 1])
            self.blocks[offset] = values, int.from_bytes(self.data[offset+2:offset+8], 'little')
        values, bits = self.blocks[offset]
        return values[(bits >> (3 * ((y % 4)*4 + x % 4))) & 7]

    def sample(self, u, v):
        x, y = u*self.w-.5, v*self.h-.5
        ix, iy = math.floor(x), math.floor(y)
        fx, fy = round((x-ix)*256)/256, round((y-iy)*256)/256
        return sum(wx * wy * self.pixel(ix+dx, iy+dy)
                   for dx, wx in ((0, 1-fx), (1, fx)) for dy, wy in ((0, 1-fy), (1, fy)))


def compare(varyings, samples, depth, mask, camera, x0, y0):
    w, h, vary = native(varyings, (2,), 16)
    sw, sh, sampled = native(samples, (2,), 16)
    fw, fh, source = native(depth, (33, 34), 4)
    if (sw, sh) != (w, h) or x0 < 0 or y0 < 0 or x0+w > fw or y0+h > fh:
        raise ValueError('ROI and reference dimensions differ')
    cb = camera.read_bytes()
    if len(cb) < 368:
        raise ValueError('camera constant buffer incomplete')
    a, b, ox, oy = struct.unpack_from('<4f', cb, 352)
    mip0 = BC4(mask)
    name = re.sub(r'_m0_', '_m9_', mask.name)
    if name == mask.name:
        raise ValueError('mask mip path must contain _m0_')
    mip9 = BC4(mask.with_name(name))
    maxima = [0.0]*8
    mismatches = [0]*8
    covered = nonfinite = 0
    # PC baseline mask error is <=1/255. This CPU bilinear/BC4 model is approximate.
    tolerances = [0.001, 0.001, 0.001, 0.001, 0, 0, 1/255, 1/255]
    for y in range(h):
        for x in range(w):
            uv = struct.unpack_from('<4f', vary, 16*(y*w+x))
            values = struct.unpack_from('<4f', sampled, 16*(y*w+x))
            if uv[0] == -99999 or values[0] == -99999:
                continue
            if not all(math.isfinite(v) for v in (*uv, *values)):
                nonfinite += 1
                continue
            covered += 1
            u, v = (x0+x+.5)/fw, (y0+y+.5)/fh
            expected_vary = (u, v, -(2*u-1)*a-ox, (1-2*v)*b-oy)
            xx, yy = max(0, min(fw-1, int(uv[0]*fw))), max(0, min(fh-1, int(uv[1]*fh)))
            rg = struct.unpack_from('<2e', source, 4*(yy*fw+xx))
            expected_sample = (*rg, mip0.sample(*uv[:2]), mip9.sample(*uv[:2]))
            for c, (actual, expected) in enumerate(zip((*uv, *values), (*expected_vary, *expected_sample))):
                error = abs(actual-expected)
                maxima[c] = max(maxima[c], error)
                mismatches[c] += error > tolerances[c]
    return dict(roi=[x0, y0, w, h], full_size=[fw, fh], covered=covered, nonfinite=nonfinite,
                channels=['Generic0.x','Generic0.y','Generic1.x','Generic1.y',
                          'depth.r','depth.g','mask_screenUV_LOD0','mask_screenUV_LOD9'],
                max_abs_error=maxima, tolerance=tolerances, points_above_tolerance=mismatches,
                limitations='Affine camera relation for the documented interface; controlled mask samples, approximate BC4/filter model. No automatic root-cause verdict.')


def compare_varying_control(path, camera, full_width, full_height, x0, y0):
    """Identity GS control has no sample pass; compare only its four varyings."""
    width, height, data = native(path, (2,), 16)
    if x0 < 0 or y0 < 0 or x0+width > full_width or y0+height > full_height:
        raise ValueError('control ROI outside reference')
    cb = camera.read_bytes()
    if len(cb) < 368:
        raise ValueError('camera constant buffer incomplete')
    a, b, ox, oy = struct.unpack_from('<4f', cb, 352)
    maxima, mismatches = [0.0]*4, [0]*4
    covered = nonfinite = 0
    for y in range(height):
        for x in range(width):
            actual = struct.unpack_from('<4f', data, 16*(y*width+x))
            if actual[0] == -99999:
                continue
            if not all(math.isfinite(v) for v in actual):
                nonfinite += 1
                continue
            covered += 1
            u, v = (x0+x+.5)/full_width, (y0+y+.5)/full_height
            expected = (u, v, -(2*u-1)*a-ox, (1-2*v)*b-oy)
            for c, (observed, reference) in enumerate(zip(actual, expected)):
                error = abs(observed-reference)
                maxima[c] = max(maxima[c], error)
                mismatches[c] += error > 0.001
    return dict(covered=covered, nonfinite=nonfinite, max_abs_error=maxima,
                points_above_tolerance=mismatches, tolerance=0.001)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('varyings', 'samples', 'depth', 'mask-mip0', 'camera-cbuf', 'output'):
        parser.add_argument('--'+key, required=True, type=Path)
    parser.add_argument('--roi-x', required=True, type=int)
    parser.add_argument('--roi-y', required=True, type=int)
    parser.add_argument('--identity-varyings', type=Path)
    parser.add_argument('--linear-varyings', type=Path)
    parser.add_argument('--linear-samples', type=Path)
    args = parser.parse_args()
    if bool(args.linear_varyings) != bool(args.linear_samples):
        parser.error('linear control requires both varying and sample exports')
    result = compare(args.varyings, args.samples, args.depth, args.mask_mip0,
                     args.camera_cbuf, args.roi_x, args.roi_y)
    if args.identity_varyings:
        result['identity_control'] = compare_varying_control(args.identity_varyings,
            args.camera_cbuf, *result['full_size'], args.roi_x, args.roi_y)
    if args.linear_varyings:
        result['linear_control'] = compare(args.linear_varyings, args.linear_samples,
            args.depth, args.mask_mip0, args.camera_cbuf, args.roi_x, args.roi_y)
    args.output.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

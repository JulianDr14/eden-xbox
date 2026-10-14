# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode bounded native RT tiles. Changes identify draws, not the cause of shading."""
import argparse
import csv
import json
import math
import struct
from pathlib import Path


def small_float(bits, mantissa_bits):
    exponent, mantissa = bits >> mantissa_bits, bits & ((1 << mantissa_bits) - 1)
    if exponent == 31:
        return math.nan if mantissa else math.inf
    return math.ldexp(mantissa / (1 << mantissa_bits), -14) if exponent == 0 else math.ldexp(
        1 + mantissa / (1 << mantissa_bits), exponent - 15)


def texel(data, dxgi):
    if dxgi in (27, 28, 29):
        return tuple(v / 255 for v in data)
    if dxgi in (87, 90, 91):
        b, g, r, a = data
        return r / 255, g / 255, b / 255, a / 255
    word, = struct.unpack('<I', data)
    if dxgi in (23, 24):
        return (word & 1023) / 1023, ((word >> 10) & 1023) / 1023, ((word >> 20) & 1023) / 1023, (word >> 30) / 3
    if dxgi == 26:
        return small_float(word & 2047, 6), small_float((word >> 11) & 2047, 6), small_float(word >> 22, 5), 1
    if dxgi in (39, 41):
        value, = struct.unpack('<f', data)
        return value, value, value, 1
    if dxgi == 44:
        value = (word & 0xffffff) / 16777215
        return value, value, value, 1
    if dxgi in (33, 34):
        r, g = struct.unpack('<ee', data)
        return r, g, 0, 1
    raise ValueError(f'Unsupported DXGI {dxgi}')


def load_tiles(index_path):
    raw = index_path.with_suffix('.bin').read_bytes()
    tiles = []
    expected_offset = 0
    with index_path.open(newline='', encoding='utf-8') as source:
        for row in csv.DictReader(source):
            for key in ('draw', 'dxgi', 'x', 'y', 'width', 'height', 'offset'):
                row[key] = int(row[key])
            if 'target' in row:
                row['target'] = int(row['target'])
                if not 0 <= row['target'] < 8:
                    raise ValueError('Invalid color target')
            row['subresource'] = int(row.get('subresource', 0))
            width, height, offset = row['width'], row['height'], row['offset']
            if not 1 <= width <= 64 or not 1 <= height <= 64 or offset != expected_offset:
                raise ValueError('Invalid tile bounds or noncontiguous offset')
            expected_offset += width * height * 4
            if expected_offset > len(raw):
                raise ValueError('Truncated tile data')
            row['data'] = raw[offset:expected_offset]
            tiles.append(row)
    if len(raw) != expected_offset or len(tiles) > 2048:
        raise ValueError('Unexpected trailing bytes or record count')
    return tiles


def write_bmp(path, tile):
    width, height = tile['width'], tile['height']
    pixels = bytearray()
    def byte(value):
        return round(max(0, min(1, value)) * 255) if math.isfinite(value) else 255
    for y in reversed(range(height)):
        for x in range(width):
            offset = (y * width + x) * 4
            r, g, b, a = texel(tile['data'][offset:offset + 4], tile['dxgi'])
            pixels.extend((byte(b), byte(g), byte(r), byte(a)))
    header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
    header += struct.pack('<IiiHHIIiiII', 40, width, height, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    path.write_bytes(header + pixels)


def summarize(tiles, pixel=None, export_dir=None, include_unchanged=False):
    previous = {}
    result = []
    for tile in tiles:
        key = (tile.get('target', 0), tile.get('subresource', 0)) + tuple(
            tile[k] for k in ('address', 'dxgi', 'x', 'y', 'width', 'height'))
        old = previous.get(key)
        current = tile['data']
        changed = None if old is None else sum(current[i:i+4] != old[1][i:i+4] for i in range(0, len(current), 4))
        previous[key] = (tile['draw'], current)
        if changed == 0 and not include_unchanged:
            continue
        record = {k: v for k, v in tile.items() if k != 'data'}
        record['changed_pixels'] = changed
        record['changed_texels'] = changed
        record['previous_draw'] = None if old is None else old[0]
        if pixel:
            x, y = pixel[0] - tile['x'], pixel[1] - tile['y']
            if 0 <= x < tile['width'] and 0 <= y < tile['height']:
                offset = (y * tile['width'] + x) * 4
                record['pixel_rgba_native'] = texel(current[offset:offset+4], tile['dxgi'])
        if export_dir:
            write_bmp(export_dir / f"draw_{tile['draw']:04d}_{tile['address']}_rt{tile.get('target', 0)}.bmp", tile)
        result.append(record)
    return result


NOTE = ('Exact native ROI changes between sampled direct draws. Clears, copies, compute '
        'and indirect draws between samples can also cause changes; a changed region '
        'does not identify an incorrect draw.')


def analyze(index):
    return {'file': str(index), 'records': summarize(load_tiles(index), include_unchanged=True),
            'note': NOTE}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('index', type=Path)
    parser.add_argument('--pixel', nargs=2, type=int)
    parser.add_argument('--export-dir', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.export_dir:
        args.export_dir.mkdir(parents=True, exist_ok=True)
    records = summarize(load_tiles(args.index), args.pixel, args.export_dir,
                        include_unchanged=bool(args.output))
    if args.output:
        args.output.write_text(json.dumps({'file': str(args.index), 'records': records,
                                          'note': NOTE}, indent=2) + '\n', encoding='utf-8')
    else:
        print(json.dumps(records, indent=2))

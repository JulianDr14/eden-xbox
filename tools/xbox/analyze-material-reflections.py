# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read six isolated reflection probes. Differences are evidence, not a diagnosis."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import re
import struct

spec = importlib.util.spec_from_file_location(
    'native_intermediates', Path(__file__).with_name('analyze-material-intermediates.py'))
native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(native)
spec = importlib.util.spec_from_file_location(
    'native_tiles', Path(__file__).with_name('analyze-material-probe.py'))
tiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tiles)
CHANNELS = ('direction_lod', 'implicit', 'level_zero', 'ambient_lut', 'gradient_x', 'gradient_y')
CFG_CHANNELS = ('cfg_cube', 'cfg_lut_power', 'cfg_direction', 'cfg_material', 'cfg_ambient', 'cfg_scaled')
BRANCH_CHANNELS = ('branch_bypass_first', 'branch_bypass_second', 'branch_bypass_both',
                   'branch_predicate_first', 'branch_predicate_second', 'branch_predicate_both')


def load(directory, prefix, channels=CHANNELS):
    result = {}
    shape = None
    for channel in channels:
        files = list(directory.glob(prefix + '_gpu_reflection_' + channel + '_x*_y*.raw'))
        if len(files) != 1:
            raise ValueError(f'Expected one {channel} export, got {len(files)}')
        path = files[0]
        match = re.search(r'_x(\d+)_y(\d+)\.raw$', path.name)
        width, height, data = native.native(path, (2,), 16)
        if width > 256 or height > 128:
            raise ValueError('Reflection ROI exceeds capture limits')
        current = (width, height, int(match[1]), int(match[2]))
        if shape is not None and current != shape:
            raise ValueError('Probe dimensions/origins differ')
        shape = current
        result[channel] = list(struct.iter_unpack('<4f', data))
    return shape, result


def summarize(directory, prefix):
    channels = CFG_CHANNELS if list(directory.glob(prefix + '_gpu_reflection_cfg_cube_x*_y*.raw')) else CHANNELS
    if list(directory.glob(prefix + '_gpu_reflection_branch_bypass_first_x*_y*.raw')):
        channels = BRANCH_CHANNELS
    shape, probes = load(directory, prefix, channels)
    total = shape[0] * shape[1]
    covered, nonfinite, inconsistent = [], 0, 0
    for i in range(total):
        missing = [any(value == -99999.0 for value in probes[c][i]) for c in channels]
        if any(missing):
            inconsistent += not all(missing)
            continue
        if not all(math.isfinite(v) for c in channels for v in probes[c][i]):
            nonfinite += 1
            continue
        covered.append(i)
    lod = [probes['direction_lod'][i][3] for i in covered] if channels == CHANNELS else []
    differences = [max(abs(a-b) for a, b in zip(probes['implicit'][i],
                                               probes['level_zero'][i])) for i in covered] if channels == CHANNELS else []
    report = dict(prefix=prefix, width=shape[0], height=shape[1], x=shape[2], y=shape[3],
                finite_covered=len(covered), uncovered=total-len(covered)-nonfinite,
                nonfinite=nonfinite, inconsistent_coverage=inconsistent,
                lod_range=[min(lod), max(lod)] if lod else None,
                implicit_vs_level_zero_max=max(differences) if differences else None,
                note='Compare with PC replay of these exact inputs; this alone does not prove a LOD bug.')
    report['mode'] = ('branch_controls' if channels == BRANCH_CHANNELS else
                      'original_cfg' if channels == CFG_CHANNELS else 'authored')
    if channels != CHANNELS:
        report['ranges'] = {c: [[min(probes[c][i][k] for i in covered),
                                max(probes[c][i][k] for i in covered)] for k in range(4)]
                            for c in channels} if covered else {}
    extra = [list(directory.glob(prefix + '_gpu_reflection_' + name + '_x*_y*.raw'))
             for name in ('original_final', 'original_r11')]
    if any(extra):
        if any(len(paths) != 1 for paths in extra):
            raise ValueError('Original-format comparison requires both exports')
        arrays = []
        for paths, formats, size in zip(extra, ((2,), (26,)), (16, 4)):
            path = paths[0]
            w, h, data = native.native(path, formats, size)
            match = re.search(r'_x(\d+)_y(\d+)\.raw$', path.name)
            if (w, h, int(match[1]), int(match[2])) != shape:
                raise ValueError('Original-format comparison ROI differs')
            arrays.append(data)
        differences = []
        for i in range(total):
            value = struct.unpack_from('<4f', arrays[0], i * 16)
            if -99999 in value or not all(math.isfinite(v) for v in value):
                continue
            packed = tiles.texel(arrays[1][i*4:i*4+4], 26)
            if not all(math.isfinite(v) for v in packed):
                continue
            differences.append(max(abs(a-b) for a, b in zip(value[:3], packed[:3])))
        report['original_format_comparison'] = dict(
            finite_pixels=len(differences),
            float_vs_r11_max_delta=max(differences) if differences else None,
            note='Includes normal R11 quantization and scratch overlap; compare with exact-input PC control.')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    prefixes = sorted({p.name.split('_gpu_reflection_')[0] for p in
                       args.directory.glob('*_gpu_reflection_*_x*_y*.raw')})
    if not prefixes:
        raise ValueError('No reflection probe exports')
    results = [summarize(args.directory, prefix) for prefix in prefixes]
    text = json.dumps(results, indent=2, allow_nan=False) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf8')
    else:
        print(text)

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare F7 captures by shader pair; differences are observations, not pixel causality."""
import argparse
import importlib.util
import json
from pathlib import Path
import re

spec = importlib.util.spec_from_file_location('material_trace',
    Path(__file__).with_name('analyze-material-trace.py'))
material = importlib.util.module_from_spec(spec)
spec.loader.exec_module(material)


def variants(capture):
    groups = {}
    for draw in capture['draws']:
        group = groups.setdefault((draw['vs'], draw['ps']),
                                  {'draw_count': 0, 'states': set(), 'bindings': set()})
        group['draw_count'] += 1
        # COM pointers and trace-local indices have no meaning across captures/processes.
        # Keep counts, guest addresses, stable key hash, state, mips and sampler values.
        state = re.sub(r' PSO=\S+', '', draw['state'])
        group['states'].add(state)
        group['bindings'].add(draw['bindings'])
    return groups


def compare(samples):
    observations = []
    grouped = []
    for label, capture in samples:
        summary = material.summarize(capture)
        grouped.append(variants(capture))
        observations.append({'label': label, 'frame': capture['frame'],
                             'partial': summary['partial'],
                             'errors': summary['errors'],
                             'capabilities': capture.get('capabilities', {}),
                             'parsed_draws': summary['parsed_draws']})
    pairs = sorted(set().union(*(set(group) for group in grouped)))
    differences = []
    for pair in pairs:
        entries = [group.get(pair, {'draw_count': 0, 'states': set(), 'bindings': set()})
                   for group in grouped]
        common_states = set.intersection(*(entry['states'] for entry in entries))
        common_bindings = set.intersection(*(entry['bindings'] for entry in entries))
        if all(entry == entries[0] for entry in entries[1:]):
            continue
        differences.append({'vs': pair[0], 'ps': pair[1], 'samples': [
            {'label': label, 'draw_count': entry['draw_count'],
             'state_variants': len(entry['states']),
             'states_outside_intersection': sorted(entry['states'] - common_states),
             'bindings_outside_intersection': sorted(entry['bindings'] - common_bindings)}
            for (label, _), entry in zip(samples, entries)]})
    return {'samples': observations, 'differences': differences,
            'comparable_complete': all(not item['partial'] for item in observations),
            'limits': 'Shader pairs may draw multiple objects. A missing pair in a partial '
                      'capture is not evidence of disappearance. No camera distance, actual '
                      'sampled LOD, cascade index or responsible pixel is inferred.'}


def load_sample(argument):
    # LABEL=PATH@INDEX (INDEX defaults to 0). Split at the rightmost @ only when numeric.
    label, separator, value = argument.partition('=')
    if not separator or not label or not value:
        raise ValueError('Expected LABEL=PATH[@INDEX]')
    path, marker, index = value.rpartition('@')
    if not marker or not index.isdigit():
        path, index = value, '0'
    captures = material.captures(Path(path).read_text(encoding='utf-8', errors='replace').splitlines())
    if int(index) >= len(captures):
        raise ValueError(f'{label}: capture {index} unavailable ({len(captures)} found)')
    return label, captures[int(index)]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sample', nargs='+', help='LABEL=PATH[@INDEX], e.g. near=eden_log.txt@0')
    args = parser.parse_args()
    try:
        if len(args.sample) < 2:
            raise ValueError('At least two labeled captures are required')
        samples = [load_sample(argument) for argument in args.sample]
        if len({label for label, _ in samples}) != len(samples):
            raise ValueError('Labels must be unique')
        print(json.dumps(compare(samples), indent=2))
    except (ValueError, OSError) as error:
        parser.error(str(error))

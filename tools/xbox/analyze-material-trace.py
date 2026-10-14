# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve bounded D3D12 draw metadata. Never infer pixel causality from bindings."""
import argparse
import json
import re
from pathlib import Path


def captures(lines):
    result = []
    capture = None
    records = {}
    bindings = {}
    capabilities = {}
    for line in lines:
        capability = re.search(r'D3D12: (adapter |highest shader model |wave ops |'
                               r'native 16-bit shader ops |typed UAV loads: |'
                               r'texture cache runtime ready |pipeline cache ready )(.*)', line)
        if capability:
            capabilities[capability[1].strip()] = capability[2]
        begin = re.search(r'D3D12 material frame (\d+): begin', line)
        if begin:
            capture = {'frame': int(begin[1]), 'complete': False, 'draws': [], 'errors': [],
                       'capabilities': capabilities.copy()}
            result.append(capture)
            records, bindings = {}, {}
            continue
        if capture is None:
            continue
        if 'D3D12 material complete:' in line:
            capture.update({k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)})
            capture['complete'] = True
            capture['partial'] = bool(capture.get('omitted_draws') or capture.get('dropped_lines') or
                                      capture.get('intervals_complete', 1) == 0)
            capture = None
            continue
        record = re.search(r'D3D12 trace #(\d+)(.*)', line)
        if not record:
            continue
        index, body = int(record[1]), record[2]
        kind = 'samplers' if body.startswith(' samplers') else 'state'
        reference = re.search(r' (state|samplers)_ref=#(\d+)$', body)
        if reference:
            source = records.get((reference[1], int(reference[2])))
            if source is None or int(reference[2]) >= index:
                capture['errors'].append(f'#{index}: unresolved/forward {reference[1]} reference')
                continue
            body = body[:reference.start()] + source
        payload_at = body.find(' samplers: ' if kind == 'samplers' else ' | VS ')
        if payload_at >= 0:
            records[kind, index] = body[payload_at:]
        if kind == 'samplers':
            bindings[index] = body
            continue
        shaders = re.search(r'\| VS (\w+) PS (\w+)', body)
        if shaders:
            capture['draws'].append({'index': index, 'vs': shaders[1], 'ps': shaders[2],
                'state': body, 'bindings': bindings.get(index, '')})
    return result


def summarize(capture):
    groups = {}
    nulls = []
    for draw in capture['draws']:
        group = groups.setdefault(draw['ps'], {'draws': 0, 'vs': set(), 'resources': set(),
                                               'formats': set(), 'bias': set()})
        group['draws'] += 1
        group['vs'].add(draw['vs'])
        group['resources'].update(re.findall(r'srv=\[@([0-9a-f]+)', draw['bindings']))
        group['formats'].update(re.findall(r'\| tex (\d+)', draw['state']))
        group['bias'].update(re.findall(r'bias enable=(\d+ guest=\S+ host=\S+ clamp=\S+ slope=\S+)', draw['state']))
        for binding in re.findall(r'srv=\[([^\]]*null=true[^\]]*)\]', draw['bindings']):
            nulls.append({'draw': draw['index'], 'ps': draw['ps'], 'binding': binding})
    for group in groups.values():
        for field in ('vs', 'resources', 'formats', 'bias'):
            group[field] = sorted(group[field])
    result = {k: v for k, v in capture.items() if k != 'draws'}
    result['partial'] = result.get('partial', True) or bool(result['errors'])
    result.update({'parsed_draws': len(capture['draws']), 'shaders': groups, 'null_srvs': nulls,
                   'causality': 'Bindings do not establish which draw produced a dark pixel.'})
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    print(json.dumps([summarize(c) for c in captures(
        args.log.read_text(encoding='utf-8', errors='replace').splitlines())], indent=2))

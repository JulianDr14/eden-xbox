# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare SSA dependency trees of SV_Position in two dxc -dumpbin listings.

Explicit signature IDs avoid guessing IDs from packed output registers. Equal trees
do not prove equality of the buffers bound at runtime, rasterization or guest pixels.
"""
import argparse
from functools import lru_cache
import hashlib
import json
import re
from pathlib import Path


def position_graph(text, signature_id):
    nodes = dict(re.findall(r'^\s*%(\d+) = (.*?)(?:  ;.*)?$', text, re.MULTILINE))
    cbuffers = dict(re.findall(r';\s*(\w+)\s+cbuffer\s+NA\s+NA\s+CB(\d+)\s+cb', text))
    cbuffers = {index: name for name, index in cbuffers.items()}
    metadata = dict(re.findall(r'^!(\d+) = (.*)$', text, re.MULTILINE))

    @lru_cache(None)
    def expand_metadata(index):
        return re.sub(r'!(\d+)', lambda match: '!(' + expand_metadata(match[1]) + ')', metadata[index])

    @lru_cache(None)
    def expand(index):
        expression = nodes[index]
        handle = re.search(r'@dx.op.createHandle\(i32 57, i8 2, i32 (\d+), i32 (\d+), i1 false\)', expression)
        if handle:
            # Normalize by original cbuffer name, preserving its actual register index.
            name = cbuffers[handle[1]]
            expression = 'HANDLE ' + name + ('' if name == 'runtime_data' else ' index=' + handle[2])
        # Resolve values as well as names: precise=true and precise=false must not compare equal.
        expression = re.sub(r'!(\d+)', lambda match: '!(' + expand_metadata(match[1]) + ')', expression)
        return re.sub(r'%(\d+)\b', lambda match: '(' + expand(match[1]) + ')', expression)

    roots = dict(re.findall(r'@dx.op.storeOutput.f32\(i32 5, i32 ' + str(signature_id) +
                           r', i32 0, i8 (\d), float %(\d+)\)', text))
    if set(roots) != {'0', '1', '2', '3'}:
        raise ValueError('Expected four SSA-valued Position stores; unsupported listing/signature')
    return {component: expand(index) for component, index in roots.items()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prepass', type=Path)
    parser.add_argument('color', type=Path)
    parser.add_argument('--pre-position', type=int, required=True)
    parser.add_argument('--color-position', type=int, required=True)
    args = parser.parse_args()
    first = position_graph(args.prepass.read_text(encoding='utf-8-sig'), args.pre_position)
    second = position_graph(args.color.read_text(encoding='utf-8-sig'), args.color_position)
    report = {component: {'identical': first[component] == second[component],
        'pre_hash': hashlib.sha256(first[component].encode()).hexdigest(),
        'color_hash': hashlib.sha256(second[component].encode()).hexdigest()}
        for component in first}
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if all(value['identical'] for value in report.values()) else 1)

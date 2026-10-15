# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Expose four SSA values in a two-merge DXIL diagnostic without rewriting arithmetic.

Input/output LLVM and assembled guest-derived DXIL must stay outside the repository.
An external assembler/validator must validate the resulting module before use.
This deliberately rejects unfamiliar CFGs; it is not an LLVM optimizer.
"""
import argparse
from pathlib import Path
import re


def redirect(source, values, body, merge, exit_block, zero, early):
    if len(values) != 4 or not all(re.fullmatch(r'%\d+', v) for v in values):
        raise ValueError('Expected four numeric SSA values')
    if 'probe_' in source:
        raise ValueError('Input already instrumented')
    # Verify each new phi edge against the existing predecessors.
    for block, predecessors in ((merge, (body, zero)), (exit_block, (merge, early))):
        header = re.search(rf'^; <label>:{block}\s+; preds = ([^\n]+)$', source, re.M)
        if not header or set(re.findall(r'%\d+', header[1])) != {f'%{p}' for p in predecessors}:
            raise ValueError('Unexpected merge predecessors')
    for v in values:
        if not re.search(rf'^  {re.escape(v)} = ', source, re.M):
            raise ValueError('Missing SSA definition')
    for block, from_block, zero_block, prefix, inputs in (
        (merge, body, zero, 'probe_merge', values),
        (exit_block, merge, early, 'probe_exit', [f'%probe_merge{i}' for i in range(4)]),
    ):
        pattern = rf'(^; <label>:{block}[^\n]*\n(?:  %[^\n]* = phi [^\n]*\n)+)'
        phis = ''.join(f'  %{prefix}{i} = phi float [ 0.000000e+00, %{zero_block} ], '
                       f'[ {v}, %{from_block} ]\n' for i, v in enumerate(inputs))
        source, count = re.subn(pattern, lambda m: m[0] + phis, source, flags=re.M)
        if count != 1:
            raise ValueError('Missing phi group')
    for channel in range(4):
        pattern = rf'(@dx.op.storeOutput.f32\(i32 5, i32 0, i32 0, i8 {channel}, float )[^)]+(\))'
        source, count = re.subn(pattern, rf'\g<1>%probe_exit{channel}\2', source)
        if count != 1:
            raise ValueError('Missing or duplicate RT0 store')
    return source


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--values', nargs=4, required=True)
    for name in ('body', 'merge', 'exit-block', 'zero', 'early'):
        parser.add_argument('--' + name, type=int, required=True)
    args = parser.parse_args()
    args.output.write_text(redirect(args.input.read_text(), args.values, args.body, args.merge,
                                   args.exit_block, args.zero, args.early), encoding='utf-8')

# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Diagnostic LLVM branch controls. Bypass requires independently proven false conditions.

Never use this to patch production shaders. Assemble/validate externally and keep
guest-derived LLVM/DXIL private. Predication continues helpers after discard;
these controls are intended to distinguish early returns from texture sampling.
"""
import re


def control(source, branches, predicated=False):
    for condition, killed, continued in branches:
        if not all(isinstance(v, int) and v >= 0 for v in (condition, killed, continued)):
            raise ValueError('Expected numeric SSA/block identifiers')
        pattern = rf'^  br i1 %{condition}, label %{killed}, label %{continued}$'
        replacement = (f'  call void @dx.op.discard(i32 82, i1 %{condition})\n' if predicated else '')
        replacement += f'  br label %{continued}'
        source, count = re.subn(pattern, lambda m: replacement, source, flags=re.M)
        if count != 1:
            raise ValueError('Expected exactly one selected branch')
        # The former kill block remains unreachable, preserving its merge phi edge.
        if not re.search(rf'^; <label>:{killed}[^\n]*\n  call void @dx.op.discard\(i32 82, i1 true\)', source, re.M):
            raise ValueError('Selected branch is not a discard block')
    return source

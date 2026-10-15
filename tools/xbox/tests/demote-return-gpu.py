# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""Numerical GPU oracle across a quad containing a discarded neighbor.

Uses noperspective-clip's existing native D3D12 fixture runner and generated DXIL.
No guest data or shaders. Run after run-demote-helper.ps1 -Fixture demote-return.
"""
from pathlib import Path
import json
import struct
import subprocess
import sys

root, executable = map(Path, sys.argv[1:])
triangles = []
# Full-screen triangle, affine varying whose discard boundary cuts a 2x2 quad.
shift = 0.0008
for x, y in ((-1., -1.), (-1., 3.), (3., -1.)):
    triangles.extend((x, y, 0., 1., x-shift, 0., 0., 0.))
source = root/'helper-neighbor.bin'
source.write_bytes(struct.pack('<24f', *triangles))
output = root/'gpu-neighbor'
subprocess.run([str(executable), str(source), str(output),
                str(root/'two_returns_vs.dxil'), str(root/'two_returns_ps.dxil')], check=True)
data = (output/'native.raw').read_bytes()
checked = discarded = boundary_checked = 0
max_error = 0.
for y in range(12, 1068, 11):
    for x in list(range(16, 1904, 13)) + [960, 961, 962]:
        value = struct.unpack_from('<4f', data, 16*(y*1920+x))
        varying = (x+.5)/960 - 1 - shift
        if varying < 0:
            assert value == (-99999.,)*4, (x, y, 'discard failed', value)
            discarded += 1
            continue
        left = ((x & ~1)+.5)/960 - 1 - shift
        right = left + 1/960
        expected = (right*right-left*left, 0., varying, 1.)
        error = max(abs(a-b) for a, b in zip(value, expected))
        assert error < 2e-6, (x, y, value, expected, error)
        max_error = max(max_error, error)
        checked += 1
        boundary_checked += x == 961
assert checked > 6000 and discarded > 6000 and boundary_checked > 80
report = dict(visible_checked=checked, discarded_checked=discarded,
              visible_pixels_with_discarded_quad_neighbor=boundary_checked,
              max_abs_error=max_error)
(root/'gpu-neighbor-oracle.json').write_text(json.dumps(report, indent=2))
print('PASS helper neighbor GPU oracle:', report)

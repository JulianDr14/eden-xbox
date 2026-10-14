#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
"""GPU regression oracle for production emitter fixtures; no private assets."""
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root, executable = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
report = []
with tempfile.TemporaryDirectory(prefix='gpu-', dir=root) as directory:
    temporary = Path(directory).resolve()
    assert temporary.parent == root
    for name, positions in (
        ('positive', ((-6,-6,0,4),(6,-6,0,4),(0,6,0,2))),
        ('crossing', ((-6,-6,0,4),(6,-6,0,4),(0,6,0,-1))),
    ):
        data = b''.join(struct.pack('<8f', *p, (p[0]/p[3]+1)/2,
                                   .25, .75, (1-p[1]/p[3])/2) for p in positions)
        source = temporary/(name+'.bin')
        source.write_bytes(data)
        for order in range(2):
            for lower in range(2):
                prefix = f'order{order}_lower{lower}'
                output = temporary/f'{name}-{prefix}'
                subprocess.run([str(executable),str(source),str(output),
                    str(root/(prefix+'_vs.dxil')),str(root/(prefix+'_ps.dxil'))],check=True)
                pixels = (output/'native.raw').read_bytes()
                maxima = [0.0]*4
                covered = 0
                nonfinite = mismatches = 0
                for y in range(0,1080,4):
                    for x in range(0,1920,4):
                        actual = struct.unpack_from('<4f',pixels,16*(y*1920+x))
                        if actual[0] == -99999:
                            continue
                        covered += 1
                        if not all(math.isfinite(v) for v in actual):
                            nonfinite += 1
                            assert not lower and name == 'crossing', (name,prefix,x,y,'nonfinite value')
                            continue
                        expected = ((x+.5)/1920,.25,.75,(y+.5)/1080)
                        for c in range(4):
                            error = abs(actual[c]-expected[c])
                            if error >= .001:
                                mismatches += 1
                                assert not lower and name == 'crossing', (name,prefix,x,y,c,error)
                            maxima[c] = max(maxima[c],error)
                assert covered > 10000, ('insufficient coverage',name,prefix,covered)
                report.append(dict(case=name,fixture=prefix,covered=covered,max_abs_error=maxima,
                                   nonfinite=nonfinite,channel_mismatches=mismatches))
(root/'gpu-comparison.json').write_text(json.dumps(report,indent=2),encoding='utf8')
print('PASS eight GPU production lowering cases: lowered affine oracle, early/late W, smooth/flat unchanged; native crossing-W control errors recorded separately')

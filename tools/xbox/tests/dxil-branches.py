# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location('branches', Path(__file__).parents[1]/'controlled-dxil-branches.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
source = ('  %1 = fcmp olt float %2, 1.0\n'
          '  br i1 %1, label %3, label %4\n'
          '; <label>:3 ; preds = %0\n'
          '  call void @dx.op.discard(i32 82, i1 true)\n'
          '  br label %5\n')
for predicated in (False, True):
    result = module.control(source, [(1, 3, 4)], predicated)
    assert '  br label %4' in result and '  br i1' not in result
    assert ('discard(i32 82, i1 %1)' in result) == predicated
    assert '%1 = fcmp olt float %2, 1.0' in result
for bad in (source.replace('i1 true', 'i1 false'), source.replace('label %4', 'label %7')):
    try:
        module.control(bad, [(1, 3, 4)])
    except ValueError:
        pass
    else:
        raise AssertionError('Unexpected control accepted')
print('PASS branch diagnostic: bypass/predication, original comparisons preserved, reject unfamiliar CFG')

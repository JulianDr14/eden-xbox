# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location('redirect', Path(__file__).parents[1] / 'redirect-dxil-probe.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
source = '\n'.join(f'  %{i} = fmul float 1.0, 2.0' for i in range(10, 14)) + '\n'
source += '; <label>:20 ; preds = %5, %6\n  %21 = phi float [ 0.0, %6 ], [ %10, %5 ]\n  br label %30\n'
source += '; <label>:30 ; preds = %20, %7\n  %31 = phi float [ 0.0, %7 ], [ %21, %20 ]\n'
source += '\n'.join(f'  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 {i}, float %31)' for i in range(4))
result = module.redirect(source, ['%10', '%11', '%12', '%13'], 5, 20, 30, 6, 7)
assert result.count('= phi float') == 10
assert result.count('fmul float') == source.count('fmul float')
assert result.count('br label') == source.count('br label')
assert 'float %probe_exit3)' in result
for bad, values in ((source.replace('preds = %5, %6', 'preds = %8, %6'), ['%10'] * 4),
                    (source, ['%999'] * 4), (source, ['%10'] * 3), (result, ['%10'] * 4)):
    try:
        module.redirect(bad, values, 5, 20, 30, 6, 7)
    except ValueError:
        pass
    else:
        raise AssertionError('Unsafe rewrite accepted')
print('PASS DXIL diagnostic: two merge edges, stores, arithmetic preservation, fail closed')

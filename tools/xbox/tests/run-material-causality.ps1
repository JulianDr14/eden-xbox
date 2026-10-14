# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$causalGate = New-Item -ItemType Directory -Force build-uwp/material-causality-gate
$causalVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$causalVs) { throw 'MSVC not found' }
$causalCommands = @('@echo off', ('call "' + $causalVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1')
foreach ($causalTest in @('material-trace', 'float-store')) {
    $causalLibs = if ($causalTest -eq 'float-store') { ' /link d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib' } else { '' }
    $causalCommands += ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /DNOMINMAX /I src tools\xbox\tests\' + $causalTest + '.cpp /Fo:build-uwp\material-causality-gate\ /Fe:build-uwp\material-causality-gate\' + $causalTest + '.exe' + $causalLibs)
    $causalCommands += 'if errorlevel 1 exit /b 1'
}
$causalCommands += 'build-uwp\material-causality-gate\material-trace.exe'
$causalCommands += 'if errorlevel 1 exit /b 1'
Set-Content "$($causalGate.FullName)\build.cmd" $causalCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($causalGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Material causality native gate failed' }
& python tools/xbox/tests/lighting-ir.py
if ($LASTEXITCODE -ne 0) { throw 'Scalar IR fixtures failed' }
# Native RTV-only conversion probe with deterministic positive normal float inputs.
# Execute separately from fixture generation so Python failures propagate.
& python -c "import struct,pathlib; pathlib.Path('build-uwp/material-causality-gate/input.bin').write_bytes(b''.join(struct.pack('<4f',1+i/1000,0.25+i/4000,0.125+i/8000,1) for i in range(371)))"
if ($LASTEXITCODE -ne 0) { throw 'Native probe input generation failed' }
& "$($causalGate.FullName)\float-store.exe" "$($causalGate.FullName)\input.bin" "$($causalGate.FullName)\native"
if ($LASTEXITCODE -ne 0) { throw 'Native float store gate failed' }

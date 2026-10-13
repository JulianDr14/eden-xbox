# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$probeGate = New-Item -ItemType Directory -Force build-uwp/material-probe-gate
$probeVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$probeVs) { throw 'MSVC not found' }
$probeCommands = @('@echo off', ('call "' + $probeVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /DNOMINMAX tools\xbox\tests\material-probe.cpp /Fo:build-uwp\material-probe-gate\ /Fe:build-uwp\material-probe-gate\material-probe.exe /link d3d12.lib dxgi.lib dxguid.lib')
Set-Content "$($probeGate.FullName)/build.cmd" $probeCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($probeGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'GPU tile gate build failed' }
& "$($probeGate.FullName)/material-probe.exe"
if ($LASTEXITCODE -ne 0) { throw 'GPU tile gate failed' }

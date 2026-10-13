# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone host-key/single-flight gate and D3D12 microbenchmark; no game data needed.
$ErrorActionPreference = 'Stop'
$gateVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$gateVs) { throw 'MSVC not found' }
$gateDir = New-Item -ItemType Directory -Force build-uwp/pso-cache-gate
$gateCommands = @('@echo off', ('call "' + $gateVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /Isrc tools\xbox\tests\pso-cache.cpp /Fobuild-uwp\pso-cache-gate\ /Febuild-uwp\pso-cache-gate\pso-cache.exe /link d3d12.lib dxgi.lib d3dcompiler.lib',
    'if errorlevel 1 exit /b 1', 'build-uwp\pso-cache-gate\pso-cache.exe')
Set-Content "$($gateDir.FullName)/run.cmd" $gateCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($gateDir.FullName)/run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'PSO cache gate failed' }

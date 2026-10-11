# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from the repository root after the incremental core_timing.cpp object build.
$ErrorActionPreference = 'Stop'
$pulseVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$pulseVs) { throw 'MSVC not found' }
$pulseBoost = Get-ChildItem '.cache/cpm/boost' -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (!$pulseBoost) { throw 'Bundled Boost not found' }
$pulseIncludes = Get-ChildItem "$($pulseBoost.FullName)/libs" -Directory | ForEach-Object { '/I "' + $_.FullName + '\include"' }
$pulseGate = New-Item -ItemType Directory -Force 'build-uwp/pulse-gate'
$pulseCompile = 'cl /nologo /O2 /MD /std:c++20 /EHsc /DNOMINMAX /I src ' + ($pulseIncludes -join ' ') + ' tools\xbox\tests\pulse-timing.cpp build-uwp\src\core\CMakeFiles\core.dir\core_timing.cpp.obj build-uwp\src\common\common.lib build-uwp\_deps\fmt-build\fmt.lib /Fo:build-uwp\pulse-gate\ /Fe:build-uwp\pulse-gate\pulse-timing.exe /link /OPT:REF WindowsApp.lib ntdll.lib'
$pulseCommands = @('@echo off', ('call "' + $pulseVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1', $pulseCompile)
Set-Content "$($pulseGate.FullName)/timing.cmd" -Value $pulseCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($pulseGate.FullName)\timing.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Pulse timing harness build failed' }
& "$($pulseGate.FullName)/pulse-timing.exe"
if ($LASTEXITCODE -ne 0) { throw 'Pulse timing gate failed' }

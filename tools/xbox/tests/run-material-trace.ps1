# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$traceGate = New-Item -ItemType Directory -Force 'build-uwp/material-trace-gate'
$traceVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$traceVs) { throw 'MSVC not found' }
$traceCommands = @('@echo off', ('call "' + $traceVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /I src tools\xbox\tests\material-trace.cpp /Fo:build-uwp\material-trace-gate\ /Fe:build-uwp\material-trace-gate\material-trace.exe')
Set-Content "$($traceGate.FullName)/build.cmd" $traceCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($traceGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Material trace gate build failed' }
& "$($traceGate.FullName)/material-trace.exe"
if ($LASTEXITCODE -ne 0) { throw 'Material trace gate failed' }

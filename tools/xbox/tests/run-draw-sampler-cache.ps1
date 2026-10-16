# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$samplerRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$samplerVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$samplerVs) { throw 'MSVC not found' }
$samplerDir = New-Item -ItemType Directory -Force (Join-Path $samplerRoot 'build-uwp/draw-sampler-cache-gate')
$samplerCommands = @('@echo off', ('call "' + $samplerVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + $samplerDir.FullName + '"'),
    ('cl /nologo /O2 /std:c++20 /EHsc /I"' + $samplerRoot + '\src" "' + $PSScriptRoot + '\draw-sampler-cache.cpp" /Fo:draw-sampler-cache.obj /Fe:draw-sampler-cache.exe'),
    'if errorlevel 1 exit /b 1', 'draw-sampler-cache.exe')
Set-Content (Join-Path $samplerDir 'build.cmd') $samplerCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c (Join-Path $samplerDir 'build.cmd')
if ($LASTEXITCODE -ne 0) { throw 'Draw sampler cache gate failed' }

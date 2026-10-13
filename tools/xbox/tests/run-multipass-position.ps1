# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(Mandatory)][string]$Prepass, [Parameter(Mandatory)][string]$Color)
$ErrorActionPreference = 'Stop'
$positionPrepass = (Resolve-Path -LiteralPath $Prepass).Path
$positionColor = (Resolve-Path -LiteralPath $Color).Path
$positionGate = New-Item -ItemType Directory -Force build-uwp/multipass-position-gate
$positionVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$positionVs) { throw 'MSVC not found' }
$positionCommands = @('@echo off', ('call "' + $positionVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /DNOMINMAX tools\xbox\tests\multipass-position.cpp /Fo:build-uwp\multipass-position-gate\ /Fe:build-uwp\multipass-position-gate\multipass-position.exe /link d3d12.lib dxgi.lib dxguid.lib')
Set-Content "$($positionGate.FullName)/build.cmd" $positionCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($positionGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Multipass GPU probe build failed' }
& "$($positionGate.FullName)/multipass-position.exe" $positionPrepass $positionColor
if ($LASTEXITCODE -eq 1) { throw 'Actual vertex shaders produced different GPU positions' }
if ($LASTEXITCODE -ne 0) { throw 'Multipass GPU probe failed' }

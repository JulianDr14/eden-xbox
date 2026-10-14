# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop forensic tool linked to existing production libraries; no full build.
param([string] $BuildDir = 'build-uwp')
$ErrorActionPreference = 'Stop'
$inspectorBuild = (Resolve-Path -LiteralPath $BuildDir).Path
$inspectorRepo = (Get-Location).Path
$inspectorOut = New-Item -ItemType Directory -Force (Join-Path $inspectorBuild 'shader-inspector')
$inspectorNinja = Get-Content -LiteralPath (Join-Path $inspectorBuild 'build.ninja') -Raw
$inspectorCompile = [regex]::Match($inspectorNinja, '(?ms)^' + [regex]::Escape('build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_pipeline_cache.cpp.obj:') + '.*?(?=^build |^#)')
$inspectorLink = [regex]::Match($inspectorNinja, '(?ms)^' + [regex]::Escape('build bin\eden-uwp.exe:') + '.*?(?=^build |^#)')
$inspectorIncludes = [regex]::Match($inspectorCompile.Value, '(?m)^  INCLUDES = (.*)$').Groups[1].Value.Trim()
$inspectorLibs = [regex]::Match($inspectorLink.Value, '(?m)^  LINK_LIBRARIES = (.*)$').Groups[1].Value.Trim()
if (!$inspectorIncludes -or !$inspectorLibs) { throw 'Configured UWP Ninja libraries/includes not found; build eden-uwp first' }
$inspectorVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$inspectorVs) { throw 'MSVC not found' }
$inspectorCommand = 'cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' +
    $inspectorIncludes + ' "' + $inspectorRepo + '\tools\xbox\inspect-captured-shader.cpp" src\eden_uwp\CMakeFiles\eden-uwp.dir\vma_impl.cpp.obj /Fo:shader-inspector\ /Fe:shader-inspector\inspect-captured-shader.exe /link /OPT:REF /NODEFAULTLIB:libcmt d3dcompiler.lib ' + $inspectorLibs
$inspectorCommands = @('@echo off', ('call "' + $inspectorVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + $inspectorBuild + '"'), $inspectorCommand)
Set-Content -LiteralPath (Join-Path $inspectorOut 'build.cmd') -Value $inspectorCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c (Join-Path $inspectorOut 'build.cmd')
if ($LASTEXITCODE -ne 0) { throw 'Captured shader inspector build failed' }
Write-Output (Join-Path $inspectorOut 'inspect-captured-shader.exe')

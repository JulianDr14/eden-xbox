# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop GPU component gate using the production UWP libraries. Run after the incremental build.
$ErrorActionPreference = 'Stop'
$testVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$testVs) { throw 'MSVC not found' }
$testNinja = Get-Content build-uwp/build.ninja
$testObject = ($testNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$testObject) { throw 'Configured D3D12 object missing' }
$testIncludes = ($testNinja[$testObject..($testObject+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$testLibs = ($testNinja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$testIncludes -or !$testLibs) { throw 'Configured includes or libraries missing' }
$testDir = New-Item -ItemType Directory -Force build-uwp/draw-targets-gate
$testSource = Join-Path (Get-Location) 'tools/xbox/tests/draw-targets.cpp'
$testCommands = @('@echo off', ('call "' + $testVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + (Join-Path (Get-Location) 'build-uwp') + '"'),
    ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' + $testIncludes + ' "' + $testSource + '" src\eden_uwp\CMakeFiles\eden-uwp.dir\vma_impl.cpp.obj /Fo:draw-targets-gate\ /Fe:draw-targets-gate\draw-targets.exe /link /OPT:REF /NODEFAULTLIB:libcmt ' + ' d3dcompiler.lib ' + $testLibs))
Set-Content "$($testDir.FullName)/build.cmd" $testCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($testDir.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Draw targets gate build failed' }
$testOldPath = $env:PATH
try {
    $testAppCrt = Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation
    $env:PATH = (Join-Path (Get-Location) 'build-uwp/package/layout') + ';' + $testAppCrt + ';' + $env:PATH
    & "$($testDir.FullName)/draw-targets.exe"
} finally {
    $env:PATH = $testOldPath
}
if ($LASTEXITCODE -ne 0) { throw 'Draw targets GPU gate failed' }

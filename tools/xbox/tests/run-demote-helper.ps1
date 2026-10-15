# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [ValidateSet('demote-helper', 'demote-return')]
    [string]$Fixture = 'demote-helper',
    [string]$SpirvToolsInclude = 'build-uwp/geometry-passthrough-gate',
    [string]$SpirvToolsLibrary = 'C:/glslang/lib/SPIRV-Tools.lib'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path "$SpirvToolsInclude/spirv-tools/libspirv.h") -or !(Test-Path $SpirvToolsLibrary)) {
    throw 'Provide installed SPIRV-Tools headers/library'
}
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$ninja = Get-Content build-uwp/build.ninja
$objectLine = ($ninja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$vs -or !$objectLine) { throw 'Configured toolchain missing' }
$includes = ($ninja[$objectLine..($objectLine+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$libs = ($ninja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$includes -or !$libs) { throw 'Configured includes/libraries missing' }
$gateName = $Fixture + '-gate'
$gate = New-Item -ItemType Directory -Force (Join-Path 'build-uwp' $gateName)
$source = Join-Path (Get-Location) ('tools/xbox/tests/' + $Fixture + '.cpp')
$commands = @('@echo off', ('call "'+$vs+'\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "'+(Join-Path (Get-Location) 'build-uwp')+'"'),
    ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK '+$includes+' /I"'+[IO.Path]::GetFullPath($SpirvToolsInclude)+'" "'+$source+'" /Fo:'+ $gateName +'\ /Fe:'+ $gateName +'\emitter.exe /link /OPT:REF /NODEFAULTLIB:libcmt '+$libs+' "'+[IO.Path]::GetFullPath($SpirvToolsLibrary)+'"'))
Set-Content "$($gate.FullName)/build.cmd" $commands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($gate.FullName)/build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Demote fixture build failed' }
$previousPath = $env:PATH
try {
    $crt = & "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -Command "Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation"
    $env:PATH = (Join-Path (Get-Location) 'build-uwp/package/layout')+';'+$crt+';'+$env:PATH
    & "$($gate.FullName)/emitter.exe" $gate.FullName
    if ($LASTEXITCODE -ne 0) { throw 'Demote SPIR-V/DXIL fixtures failed' }
} finally { $env:PATH = $previousPath }

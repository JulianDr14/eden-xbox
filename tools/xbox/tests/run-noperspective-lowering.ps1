# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [string]$SpirvToolsInclude = "$PSScriptRoot/../../../build-uwp/geometry-passthrough-gate",
    [string]$SpirvToolsLibrary = 'C:/glslang/lib/SPIRV-Tools.lib'
)
$ErrorActionPreference = 'Stop'
if (!(Test-Path "$SpirvToolsInclude/spirv-tools/libspirv.h") -or !(Test-Path $SpirvToolsLibrary)) {
    throw 'Provide installed SPIRV-Tools headers/library'
}
$npVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$npNinja = Get-Content build-uwp/build.ninja
$npObject = ($npNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$npVs -or !$npObject) { throw 'Configured toolchain missing' }
$npIncludes = ($npNinja[$npObject..($npObject+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$npLibs = ($npNinja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$npIncludes -or !$npLibs) { throw 'Configured includes/libraries missing' }
$npDir = New-Item -ItemType Directory -Force build-uwp/noperspective-lowering-gate
$npSource = Join-Path (Get-Location) 'tools/xbox/tests/noperspective-lowering.cpp'
$npNative = Join-Path (Get-Location) 'tools/xbox/tests/noperspective-clip.cpp'
$npCommands = @('@echo off', ('call "'+$npVs+'\VC\Auxiliary\Build\vcvarsall.bat" x64'),
 'if errorlevel 1 exit /b 1', ('cd /d "'+(Join-Path (Get-Location) 'build-uwp')+'"'),
 ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK '+$npIncludes+' /I"'+[IO.Path]::GetFullPath($SpirvToolsInclude)+'" "'+$npSource+'" /Fo:noperspective-lowering-gate\ /Fe:noperspective-lowering-gate\emitter.exe /link /OPT:REF /NODEFAULTLIB:libcmt '+$npLibs+' "'+[IO.Path]::GetFullPath($SpirvToolsLibrary)+'"'),
 'if errorlevel 1 exit /b 1',
 ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /DNOMINMAX "'+$npNative+'" /Fo:noperspective-lowering-gate\ /Fe:noperspective-lowering-gate\gpu.exe /link d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib'))
Set-Content "$($npDir.FullName)/build.cmd" $npCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($npDir.FullName)/build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Lowering gate build failed' }
$npOldPath = $env:PATH
try {
    $npCrt = & "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -Command "Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation"
    $env:PATH = (Join-Path (Get-Location) 'build-uwp/package/layout')+';'+$npCrt+';'+$env:PATH
    & "$($npDir.FullName)/emitter.exe" $npDir.FullName
    if ($LASTEXITCODE -ne 0) { throw 'SPIR-V / DXIL fixtures failed' }
    python "$PSScriptRoot/noperspective-lowering.py" $npDir.FullName "$($npDir.FullName)/gpu.exe"
    if ($LASTEXITCODE -ne 0) { throw 'GPU numerical comparison failed' }
} finally { $env:PATH = $npOldPath }

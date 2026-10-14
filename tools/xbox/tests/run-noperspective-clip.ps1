# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
param(
    [Parameter(Mandatory)][string] $InputFile,
    [string] $OutputDir = 'build-uwp/material-causality-gate/clip',
    [string] $VertexShader,
    [string[]] $ConstantBuffers = @(),
    [string] $PixelShader,
    [string] $DepthInput,
    [string] $MaskMip0,
    [string] $GeometryShader
)
$ErrorActionPreference = 'Stop'
if ($GeometryShader -and !$PixelShader) { throw 'Geometry control requires the full captured pixel replay' }
if ($VertexShader) {
    $clipExpected = if ($PixelShader) { 5 } else { 2 }
    if ($ConstantBuffers.Count -ne $clipExpected) { throw "Expected $clipExpected captured constant buffers in binding order" }
    if ($PixelShader -and (!$DepthInput -or !$MaskMip0)) { throw 'Captured pixel replay requires RG16 input and BC4 mip0 (all ten adjacent mip files)' }
} elseif ($ConstantBuffers.Count -or $PixelShader -or $DepthInput -or $MaskMip0) {
    throw 'Captured resources require VertexShader'
}
$clipBuild = New-Item -ItemType Directory -Force 'build-uwp/material-causality-gate'
$clipVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$clipVs) { throw 'MSVC not found' }
$clipCommand = @('@echo off', ('call "' + $clipVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /DNOMINMAX tools\xbox\tests\noperspective-clip.cpp /Fo:build-uwp\material-causality-gate\ /Fe:build-uwp\material-causality-gate\noperspective-clip.exe /link d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib')
$clipBatch = Join-Path $clipBuild.FullName 'build-clip.cmd'
Set-Content -LiteralPath $clipBatch -Value $clipCommand -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c $clipBatch
if ($LASTEXITCODE -ne 0) { throw 'Native clip probe build failed' }
$clipArgs = @((Resolve-Path -LiteralPath $InputFile).Path, [IO.Path]::GetFullPath($OutputDir))
if ($VertexShader) {
    $clipPixel = $PixelShader
    if (!$clipPixel) {
        $clipDxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter dxc.exe -Recurse |
            Where-Object { $_.Directory.Name -eq 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1
        if (!$clipDxc) { throw 'SDK x64 dxc.exe not found' }
        $clipPixel = Join-Path $clipBuild.FullName 'varyings.dxil'
        & $clipDxc.FullName -T ps_6_4 -E main -Fo $clipPixel tools/xbox/tests/captured-varyings.hlsl
        if ($LASTEXITCODE -ne 0) { throw 'Varyings pixel shader compilation failed' }
    }
    $clipArgs += @($VertexShader, $ConstantBuffers[0], $ConstantBuffers[1], $clipPixel)
    if ($PixelShader) { $clipArgs += @($ConstantBuffers[2], $ConstantBuffers[3], $ConstantBuffers[4], $DepthInput, $MaskMip0) }
    if ($GeometryShader) { $clipArgs += $GeometryShader }
}
& "$($clipBuild.FullName)\noperspective-clip.exe" @clipArgs
if ($LASTEXITCODE -ne 0) { throw 'Native replay failed' }

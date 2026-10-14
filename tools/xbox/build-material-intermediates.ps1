# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference='Stop'
$probeDxc=Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Filter dxc.exe -Recurse | Where-Object {$_.Directory.Name -eq 'x64'} | Sort-Object FullName -Descending | Select-Object -First 1
if (!$probeDxc) {throw 'SDK x64 dxc.exe not found'}
$probeOutput=New-Item -ItemType Directory -Force build-uwp/material-causality-gate
foreach ($probeMode in 0,1) {
 $probeName=if ($probeMode) {'samples'} else {'varyings'}
 & $probeDxc.FullName -T ps_6_4 -E main -HV 2018 -validator-version 1.4 -D "MATERIAL_SAMPLES=$probeMode" -Vn "material_${probeName}_ps" -Fh "src/video_core/renderer_d3d12/diagnostics/material_${probeName}_ps.h" -Fo "$($probeOutput.FullName)/material_${probeName}_ps.dxil" src/video_core/renderer_d3d12/diagnostics/material_intermediates.hlsl
 if ($LASTEXITCODE -ne 0) {throw 'Diagnostic PS compile failed'}
 $probeHeader="src/video_core/renderer_d3d12/diagnostics/material_${probeName}_ps.h"
 $probeLines=[IO.File]::ReadAllLines((Join-Path (Get-Location) $probeHeader)) | ForEach-Object { $_.TrimEnd() }
 [IO.File]::WriteAllText((Join-Path (Get-Location) $probeHeader), ($probeLines -join "`n").TrimEnd()+"`n", [Text.UTF8Encoding]::new($false))
}
foreach ($probeVariant in @(
 @{Name='identity_gs'; Profile='gs_6_4'; Entry='geometry'; Linear=0; Samples=0},
 @{Name='linear_gs'; Profile='gs_6_4'; Entry='geometry'; Linear=1; Samples=0},
 @{Name='linear_varyings_ps'; Profile='ps_6_4'; Entry='pixel'; Linear=1; Samples=0},
 @{Name='linear_samples_ps'; Profile='ps_6_4'; Entry='pixel'; Linear=1; Samples=1}
)) {
 $probeSymbol='material_'+$probeVariant.Name
 $probeHeader="src/video_core/renderer_d3d12/diagnostics/$probeSymbol.h"
 & $probeDxc.FullName -T $probeVariant.Profile -E $probeVariant.Entry -HV 2018 -validator-version 1.4 -D "MATERIAL_LINEAR=$($probeVariant.Linear)" -D "MATERIAL_SAMPLES=$($probeVariant.Samples)" -Vn $probeSymbol -Fh $probeHeader -Fo "$($probeOutput.FullName)/$probeSymbol.dxil" src/video_core/renderer_d3d12/diagnostics/material_interpolation.hlsl
 if ($LASTEXITCODE -ne 0) {throw "Diagnostic interpolation shader compile failed: $probeSymbol"}
 $probeLines=[IO.File]::ReadAllLines((Join-Path (Get-Location) $probeHeader)) | ForEach-Object { $_.TrimEnd() }
 [IO.File]::WriteAllText((Join-Path (Get-Location) $probeHeader), ($probeLines -join "`n").TrimEnd()+"`n", [Text.UTF8Encoding]::new($false))
}

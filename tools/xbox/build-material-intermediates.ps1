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

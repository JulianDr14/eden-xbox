# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
# Same-fork desktop Vulkan comparison; gameplay ends only when the user closes it.
param([string] $Game = 'wonder.nsp', [ValidateRange(1, 1048576)] [int] $MemoryLimitMiB = 5120)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$exe = Join-Path $repo 'build-vulkan-pc/bin/eden-cli.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'Primero ejecuta tools\xbox\build-vulkan-pc.bat.' }
if (Get-Process -Name eden-uwp,eden-cli,yuzu-cmd -ErrorAction SilentlyContinue) {
    throw 'Cierra la corrida actual antes de comparar; no se interrumpe automaticamente.'
}
$pkg = Get-AppxPackage -Name EdenEmuProject.EdenXbox
if (-not $pkg) { throw 'No se encontro el paquete de Xbox instalado en PC.' }
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$data = Join-Path $local 'eden'
$gamePath = if ([IO.Path]::IsPathRooted($Game)) { $Game } else { Join-Path $local "games\$Game" }
if (-not (Test-Path -LiteralPath $gamePath)) { throw "No existe el juego: $gamePath" }
. (Join-Path $PSScriptRoot 'process-memory-limit.ps1')
$configPath = Join-Path $data 'config/vulkan-compare.ini'
# A dedicated configuration keeps the UWP configuration intact. Defaults are forced in the opt-in
# frontend for Vulkan/1x/FIFO/fastmem0/async shaders; keys, saves and JIT profiles use existing data.
New-Item -ItemType Directory -Path (Split-Path $configPath) -Force | Out-Null
if (-not (Test-Path -LiteralPath $configPath)) {
    [IO.File]::WriteAllText($configPath, "[Renderer]`nbackend=1`n")
}
$previous = $env:EDEN_VULKAN_COMPARE_USER_DIR
try {
    $env:EDEN_VULKAN_COMPARE_USER_DIR = $data
    $app = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) `
        -ArgumentList @('-c', ('"' + $configPath + '"'), ('"' + $gamePath + '"')) -PassThru
    Set-EdenProcessMemoryLimit -ProcessId $app.Id -LimitMiB $MemoryLimitMiB
    Write-Host "Vulkan PC: limite verificado $MemoryLimitMiB MiB, PID $($app.Id). T captura 8s; Q cierra."
    Write-Host "Log separado: $(Join-Path $data 'log-vulkan-compare/eden_log.txt')"
} finally {
    $env:EDEN_VULKAN_COMPARE_USER_DIR = $previous
}

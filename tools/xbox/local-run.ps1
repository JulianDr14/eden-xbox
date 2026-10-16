# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Local iteration loop on the PC (Developer Mode): build eden-uwp, restage the package layout,
# register it loose, launch it in its AppContainer, wait for the boot to finish and print the diag.
# D3D12 behaves the same on desktop for everything tested so far, so each change is tried here
# before going to the console.
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\local-run.ps1 [-NoBuild] [-TimeoutSec 75]
#       [-BootNro path\to\payload.nro] [-RunSeconds 20] [-DebugLayer]
#       [-Game path\to\game.nsp] [-MemoryLimitMiB 5120]
#
# -RunSeconds runs payloads without the sentinels (deko3d examples, ...) for that long, then exits.
# -DebugLayer turns on the D3D12 debug layer; its errors and warnings land in eden_log.txt.
# -Game passes the user's own game dump through package-appx.ps1 (copied into LocalState
# on the first run; afterwards -Game with just the file name is enough).
# The log is left in %LOCALAPPDATA%\Packages\<family>\LocalState\eden\log\eden_log.txt (path
# printed at the end).

param([int] $TimeoutSec = 75, [switch] $NoBuild, [string] $BootNro, [int] $RunSeconds = 0,
      [switch] $DebugLayer, [string] $Game, [string[]] $BootCfg = @(),
      [ValidateRange(0, 1048576)] [int] $MemoryLimitMiB = 5120, [switch] $Library, [switch] $AllowVersionDowngrade)
$ErrorActionPreference = 'Stop'
# Match package-appx's comma-separated -BootCfg handling before applying defaults.
# Otherwise an explicit developer_hotkeys=0 later in the string gets overridden.
$BootCfg = @($BootCfg | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# The app caps its own commit at this (a job object, uwp_boot.cpp ApplyProcessMemoryLimit), so every
# session of the package is limited, also the ones launched by hand. A zero cap opts out.
$BootCfg = @($BootCfg | Where-Object { $_ -notmatch '^memory_limit_mib=' })
$BootCfg += "memory_limit_mib=$MemoryLimitMiB"
if ($Library) { $BootCfg += 'library=1'; $BootCfg += 'play=1' }
# Direct performance trials retain development Q/T; library uses normal bindings.
if (-not $Library -and -not ($BootCfg -match '^developer_hotkeys=')) { $BootCfg += 'developer_hotkeys=1' }
$r = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $BootNro) { $BootNro = "$r\tools\xbox\boot_nro\boot.nro" }
$BootNro = (Resolve-Path $BootNro).Path
$cmd = "$env:SystemRoot\System32\cmd.exe"

if (-not $NoBuild) {
    $out = & $cmd /c "`"$r\tools\xbox\build-env.bat`" cmake --build `"$r\build-uwp`" --target eden-uwp 2>&1"
    $out | Where-Object { $_ -match 'error|warning C|\[\d+/\d+\]' } | Select-Object -Last 8
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

Get-Process -Name eden-uwp -ErrorAction SilentlyContinue | Stop-Process -Force
Push-Location $r
try {
    & "$r\tools\xbox\package-appx.ps1" -BootNro $BootNro -RunSeconds $RunSeconds `
        -DebugLayer:$DebugLayer -Game $Game -BootCfg $BootCfg *> $null
} finally {
    Pop-Location
}
# The layout is recreated on every package; a same-version re-register does not re-grant the
# AppContainer read access, and the app then cannot open its own files (Load status 2).
& icacls "$r\build-uwp\package\layout" /grant "*S-1-15-2-1:(OI)(CI)RX" /T /Q | Out-Null
Add-AppxPackage -Register "$r\build-uwp\package\layout\AppxManifest.xml" -ForceApplicationShutdown `
    -ForceUpdateFromAnyVersion:$AllowVersionDowngrade

$pkg = Get-AppxPackage -Name EdenEmuProject.EdenXbox
$local = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
$diag = Join-Path $local 'eden_uwp_diag.txt'
Remove-Item -LiteralPath $diag -ErrorAction SilentlyContinue
Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!App"

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$relaunches = 0
while ((Get-Date) -lt $deadline) {
    if ((Test-Path $diag) -and (Select-String -Path $diag -Pattern 'exiting|CRASH' -Quiet)) { break }
    # The Pulse store gate ends the process on purpose and asks for a restart, which Windows refuses
    # to an app out of the foreground: launch it again once it is gone.
    if ((Test-Path $diag) -and -not (Get-Process -Name eden-uwp -ErrorAction SilentlyContinue)) {
        $asked = @(Select-String -Path $diag -Pattern 'launch the app again').Count
        if ($asked -gt $relaunches) {
            $relaunches = $asked
            Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!App"
        }
    }
    Start-Sleep -Milliseconds 500
}
Get-Content $diag
Write-Host "log: $(Join-Path $local 'eden\log\eden_log.txt')"

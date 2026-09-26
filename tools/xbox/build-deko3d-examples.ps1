# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the official deko3d examples (devkitPro switch-examples, graphics/deko3d/deko_examples) as
# one NRO per example, the GPU test payloads of phase 4. The sources are copied from the devkitPro
# install into build-uwp\payloads\deko3d (outside git); only our main.cpp replaces theirs, because
# the menu waits for input the headless boot never sends.
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\build-deko3d-examples.ps1 [-Examples 2,4]
#
# Needs devkitPro with switch-dev and deko3d (pacman -S switch-dev deko3d). Output:
# build-uwp\payloads\deko3d\deko3d_exNN.nro, to run with
#   tools\xbox\local-run.ps1 -NoBuild -BootNro <nro> -RunSeconds 20

param(
    [int[]] $Examples = @(2, 3, 4, 9, 10),
    [string] $DevkitPro = "C:\devkitPro"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$examplesSrc = Join-Path $DevkitPro "examples\switch\graphics\deko3d\deko_examples"
if (-not (Test-Path (Join-Path $examplesSrc "Makefile"))) { throw "deko3d examples not found in $examplesSrc" }
$make = Join-Path $DevkitPro "msys2\usr\bin\make.exe"
if (-not (Test-Path $make)) { throw "make not found: $make" }

$work = Join-Path $repo "build-uwp\payloads\deko3d"
New-Item -ItemType Directory -Force $work | Out-Null
foreach ($item in "Makefile", "romfs", "source") {
    Copy-Item (Join-Path $examplesSrc $item) $work -Recurse -Force
}
Copy-Item (Join-Path $PSScriptRoot "deko3d\main.cpp") (Join-Path $work "source\main.cpp") -Force
# Our own payloads on the same framework (example 10: blits and masked clears, phase 4.4).
Copy-Item (Join-Path $PSScriptRoot "deko3d\Example10_EdenBlit.cpp") (Join-Path $work "source") -Force

# The devkitPro toolchain expects its own environment; the msys2 make needs a writable temp dir
# without spaces (see docs/xbox_internal.md).
$env:DEVKITPRO = "/opt/devkitpro"
$env:TMP = $work; $env:TEMP = $work; $env:TMPDIR = $work
$env:PATH = "$DevkitPro\msys2\usr\bin;$DevkitPro\devkitA64\bin;$DevkitPro\tools\bin;$env:PATH"

Push-Location $work
try {
    foreach ($n in $Examples) {
        $name = "deko3d_ex{0:D2}" -f $n
        Write-Host "building : $name"
        & $make "TARGET=$name" "BUILD=build_$name" "DEFINES=-DEDEN_DEKO_EXAMPLE=$n" "APP_TITLE=$name"
        if ($LASTEXITCODE) { throw "make failed for example $n" }
        Write-Host "built    : $(Join-Path $work "$name.nro")"
    }
} finally {
    Pop-Location
}

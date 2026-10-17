# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CI environment setup for the uwp-x64 preset build (GitHub Actions windows-latest).
#
# Replicates what tools/xbox/build-env.bat does locally, but tolerant of a fresh runner:
# it locates VS via vswhere, imports vcvarsall x64 uwp into this PowerShell session,
# verifies we did NOT silently fall back to a desktop (non-Store-CRT) environment, and
# installs/downloads the three native tools the configure+build needs:
#   - perl + nasm : the from-source OpenSSL (-UWP target, /MD Store CRT)
#   - glslangValidator : video_core/host_shaders SPIR-V compilation at configure time
#
# Usage (from a workflow "shell: pwsh" step):
#   . ./tools/xbox/ci-setup-env.ps1          # dot-source: env lands in the current shell
#
# All state is written to $env:GITHUB_ENV / $env:GITHUB_PATH so subsequent steps
# (including separate cmd.exe steps) inherit it.

$ErrorActionPreference = "Stop"

# --- 1. Visual Studio + UWP Store-CRT environment -------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; runner has no Visual Studio?" }
$vsroot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsroot) { throw "No Visual Studio instance with the C++ toolset found." }
Write-Host "VS root: $vsroot"

# The Windows 10/11 SDK (MakeAppx/SignTool/dxil.dll live here) - record it for later steps.
$sdkRoot = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory -Filter "10.*" -ErrorAction SilentlyContinue |
           Where-Object { Test-Path "$($_.FullName)\x64\makeappx.exe" } | Sort-Object Name -Descending | Select-Object -First 1
if ($sdkRoot) { ${env:EDEN_WINSDK_BIN} = $sdkRoot.FullName } else { Write-Warning "Windows SDK bin with MakeAppx not found." }

# vcvarsall writes to stdout; capture then import into this session.
# English compiler messages matter: Ninja's /showIncludes dep-scanning matches the prefix byte-for-byte.
${env:VSLANG} = "1033"
cmd /c "`"$vsroot\VC\Auxiliary\Build\vcvarsall.bat`" x64 uwp > nul && set" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") { Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] }
}

# Verify rather than assume: vcvarsall falls back SILENTLY to a desktop x64 env when the
# "C++ (v143) Universal Windows Platform tools" component is missing. A desktop-CRT build
# compiles fine and then fails at AppContainer link/activation, so fail HERE instead.
if (($env:LIB -split ";") -notmatch "x64\\store") {
    throw @"
LIB does not point at the Store CRT - vcvarsall 'x64 uwp' silently fell back to desktop.
Install the VS component Microsoft.VisualStudio.ComponentGroup.UWP.VC
("C++ (v143) Universal Windows Platform tools"). LIB=$env:LIB
"@
}
Write-Host "Store CRT: OK (app_plat=$env:VSCMD_ARG_app_plat)"

# VS-bundled CMake + Ninja must win over anything preinstalled on the runner
# (chocolatey/scoop runners ship a CMake that predates WindowsStore preset support quirks).
$cmDir = "$vsroot\Common7\IDE\CommonExtensions\Microsoft\CMake"
${env:PATH} = "$cmDir\Ninja;$cmDir\CMake\bin;${env:PATH}"

# --- 2. native perl + nasm (from-source OpenSSL) ---------------------------------------
# Strawberry Perl portable zip - same layout build-env.bat expects (C:\Strawberry\perl\bin).
# Do NOT extract C:\Strawberry\c (its bundled old CMake shadows the VS one).
$strawZip = "C:\strawberry-perl.zip"
if (-not (Test-Path "C:\Strawberry\perl\bin\perl.exe")) {
    $url = "https://strawberryperl.com/download/5.32.1.1/strawberry-perl-5.32.1.1-64bit-portable.zip"
    Write-Host "Downloading Strawberry Perl portable..."
    Invoke-WebRequest -Uri $url -OutFile $strawZip -UseBasicParsing
    Expand-Archive -Path $strawZip -DestinationPath "C:\StrawberryTmp" -Force
    New-Item -ItemType Directory -Force "C:\Strawberry" | Out-Null
    Copy-Item "C:\StrawberryTmp\perl" "C:\Strawberry\perl" -Recurse -Force
    Copy-Item "C:\StrawberryTmp\c\bin\*.exe" "C:\Strawberry\perl\bin" -Force -ErrorAction SilentlyContinue
    # gzip.exe lives under c\bin in the portable zip; OpenSSL's Configure wants it on PATH.
}
${env:PATH} = "C:\Strawberry\perl\bin;${env:PATH}"

if (-not (Get-Command nasm.exe -ErrorAction SilentlyContinue)) {
    $nasmVer = "3.01"
    $nasmZip = "C:\nasm.zip"
    Invoke-WebRequest -Uri "https://www.nasm.us/pub/nasm/releasebuilds/$nasmVer/win64/nasm-$nasmVer-win64.zip" -OutFile $nasmZip -UseBasicParsing
    Expand-Archive -Path $nasmZip -DestinationPath "C:\nasm-tmp" -Force
    Move-Item "C:\nasm-tmp\nasm-$nasmVer" "C:\Program Files\NASM" -Force
}
${env:PATH} = "C:\Program Files\NASM;${env:PATH}"

# --- 3. glslangValidator (host shaders SPIR-V at configure time) ------------------------
# Recent glslang releases ship only glslang.exe; copy it under the validator name exactly
# as the Vulkan SDK does (the top-level CMake find_program looks for glslangValidator).
if (-not (Get-Command glslangValidator.exe -ErrorAction SilentlyContinue)) {
    $gl = "C:\glslang"
    $rel = Invoke-RestMethod -Uri "https://api.github.com/repos/KhronosGroup/glslang/releases/latest" -Headers @{ "User-Agent" = "eden-ci" }
    $asset = $rel.assets | Where-Object { $_.name -like "glslang-*win64.zip" } | Select-Object -First 1
    if (-not $asset) { throw "No win64 asset on glslang release '$($rel.tag_name)'" }
    Write-Host "Downloading glslang $($rel.tag_name)..."
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile "C:\glslang.zip" -UseBasicParsing
    Expand-Archive -Path "C:\glslang.zip" -DestinationPath "C:\glslang-tmp" -Force
    Move-Item "C:\glslang-tmp\install" $gl -Force
    if (-not (Test-Path "$gl\bin\glslangValidator.exe")) {
        Copy-Item "$gl\bin\glslang.exe" "$gl\bin\glslangValidator.exe" -Force
    }
}
${env:PATH} = "C:\glslang\bin;${env:PATH}"

# --- 4. persist for later steps ---------------------------------------------------------
"EDEN_VSROOT=$vsroot"        | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"EDEN_WINSDK_BIN=$env:EDEN_WINSDK_BIN" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
"PATH=$env:PATH"             | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
foreach ($v in "INCLUDE","LIB","LIBPATH","SystemRoot") {
    if (Test-Path "Env:$v") { "${v}=$(Get-Item "Env:$v").Value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
}

# Final smoke check: everything build-env.bat checks for.
foreach ($t in "cmake.exe","ninja.exe","cl.exe","perl.exe","nasm.exe","glslangValidator.exe") {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "required tool missing after setup: $t" }
    Write-Host "  ${t}: ok"
}
Write-Host "UWP build environment ready."

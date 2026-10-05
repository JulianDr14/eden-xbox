# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Mesa's spirv_to_dxil.dll as a UWP (Store CRT, AppContainer) DLL for the D3D12 renderer's
# shader path. package-appx.ps1 picks it up from the default -WorkDir. Run from PowerShell:
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\build-spirv-to-dxil.ps1
#
# Needs Python 3 on PATH and Visual Studio with the C++ x64 toolset and the UWP (Store CRT)
# component. Mesa sources, the venv and the build tree live in -WorkDir, outside the repo; the DLL is
# a build artifact and is never committed.

param(
    [string] $WorkDir = (Join-Path $PSScriptRoot "..\..\..\mesa-build"),
    [string] $MesaVersion = "26.2.3",
    # Wipe and reconfigure the build tree (after changing options or the Mesa version).
    [switch] $Reconfigure
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$WorkDir = (Resolve-Path $WorkDir).Path
$src = Join-Path $WorkDir "mesa-$MesaVersion"
$build = Join-Path $WorkDir "build-uwp"
$venv = Join-Path $WorkDir "venv"

# --- Sources ---------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $src "meson.build"))) {
    $tarball = Join-Path $WorkDir "mesa-$MesaVersion.tar.xz"
    if (-not (Test-Path $tarball)) {
        Write-Host "downloading Mesa $MesaVersion ..."
        Invoke-WebRequest "https://archive.mesa3d.org/mesa-$MesaVersion.tar.xz" -OutFile $tarball
    }
    Write-Host "extracting $tarball ..."
    # tar reports errors for the few symlinks in the tarball (CI files, not needed here): the check
    # below is what decides whether extraction worked.
    & tar.exe -xf $tarball -C $WorkDir 2>$null
    if (-not (Test-Path (Join-Path $src "meson.build"))) { throw "extracting Mesa failed" }
}

# --- Local patches (idempotent) ---------------------------------------------------------------
function Patch-File([string] $rel, [string] $old, [string] $new) {
    $path = Join-Path $src $rel
    $text = [IO.File]::ReadAllText($path)
    if ($text.Contains($new)) { return }
    if (-not $text.Contains($old)) { throw "$rel changed upstream: the patch no longer applies" }
    [IO.File]::WriteAllText($path, $text.Replace($old, $new))
    Write-Host "patched  : $rel"
}
# With no gallium/vulkan driver, NIR is built as an empty stub, but spirv_to_dxil needs all of it.
Patch-File "src\compiler\nir\meson.build" `
    "with_nir_headers_only = not with_gfx_compute or (" `
    "with_nir_headers_only = (not with_gfx_compute and not with_spirv_to_dxil) or ("
# GetConsoleWindow does not exist for UWP apps (Mesa already skips it for the Xbox GDK).
Patch-File "src\util\os_misc.c" `
    "#if !defined(_GAMING_XBOX)`n   if(GetConsoleWindow()" `
    "#if !defined(_GAMING_XBOX) && !defined(MESA_UWP)`n   if(GetConsoleWindow()"
# Linked-pipeline entry point (eden_spirv_to_dxil_pipeline): the sources live in the repo and are
# copied on every run, so edits to them reach the next build.
$s2d = Join-Path $src "src\microsoft\spirv_to_dxil"
Copy-Item (Join-Path $PSScriptRoot "mesa\eden_pipeline.c") $s2d -Force
Copy-Item (Join-Path $PSScriptRoot "mesa\eden_integer_sampling.h") $s2d -Force
Copy-Item (Join-Path $PSScriptRoot "..\..\externals\spirv-to-dxil\include\eden_spirv_to_dxil.h") $s2d -Force
Patch-File "src\microsoft\spirv_to_dxil\spirv_to_dxil.def" `
    "    spirv_to_dxil_get_version`n" `
    "    spirv_to_dxil_get_version`n    eden_spirv_to_dxil_pipeline`n"
# v2: integer texture sampling. A separate symbol, so an older DLL is detected instead of
# compiling shaders that would not read the sampler table.
Patch-File "src\microsoft\spirv_to_dxil\spirv_to_dxil.def" `
    "    eden_spirv_to_dxil_pipeline`n" `
    "    eden_spirv_to_dxil_pipeline`n    eden_spirv_to_dxil_pipeline_v2`n"
Patch-File "src\microsoft\spirv_to_dxil\meson.build" `
    "      'spirv_to_dxil.h',`n" `
    "      'spirv_to_dxil.h',`n      'eden_pipeline.c',`n      'eden_spirv_to_dxil.h',`n"

# --- Python tools -----------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $venv "Scripts\meson.exe"))) {
    Write-Host "creating the meson venv ..."
    & python -m venv $venv
    & (Join-Path $venv "Scripts\pip.exe") install --quiet meson mako pyyaml packaging
    if ($LASTEXITCODE) { throw "pip install failed" }
}

# --- MSVC environment -------------------------------------------------------------------------
# Meson runs in a desktop x64 environment (its sanity checks run programs); the Store CRT and
# AppContainer come in through the cross file's link arguments instead.
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
$vsRoot = & vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw "no Visual Studio instance with the C++ toolset was found" }
$env:VSLANG = "1033"
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvarsall.bat"
foreach ($line in (& cmd.exe /c "`"$vcvars`" x64 >nul && set")) {
    if ($line -match "^([^=]+)=(.*)$") { Set-Item "env:$($Matches[1])" $Matches[2] }
}
$env:PATH = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$venv\Scripts;$env:PATH"
if (-not $env:VCToolsInstallDir) { throw "vcvarsall x64 failed" }

# --- Configure ------------------------------------------------------------------------------
$storeLib = (Join-Path $env:VCToolsInstallDir "lib\x64\store").Replace("\", "/")
if (-not (Test-Path $storeLib)) { throw "the Store CRT is missing: install the UWP C++ tools" }
$linkArgs = "['/LIBPATH:$storeLib', 'WindowsApp.lib', '/APPCONTAINER']"
$cross = Join-Path $WorkDir "uwp-x64.cross.ini"
@"
[binaries]
c = 'cl'
cpp = 'cl'
ar = 'lib'

[built-in options]
c_args = ['/DMESA_UWP']
cpp_args = ['/DMESA_UWP']
c_winlibs = []
cpp_winlibs = []
c_link_args = $linkArgs
cpp_link_args = $linkArgs

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
"@ | Set-Content -Encoding ascii $cross

if ($Reconfigure -or -not (Test-Path (Join-Path $build "build.ninja"))) {
    $setup = @("setup", $build, $src, "--cross-file", $cross,
        "--backend=ninja", "--buildtype=release", "--wrap-mode=nodownload", "-Db_vscrt=md",
        "-Dplatforms=windows", "-Dgallium-drivers=", "-Dvulkan-drivers=", "-Dspirv-to-dxil=true",
        "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled", "-Degl=disabled",
        "-Dglx=disabled", "-Dgbm=disabled",
        "-Dllvm=disabled", "-Dmicrosoft-clc=disabled", "-Dmesa-clc=auto",
        "-Dshader-cache=disabled", "-Dzlib=disabled", "-Dzstd=disabled", "-Dexpat=disabled",
        "-Dxmlconfig=disabled", "-Dvalgrind=disabled", "-Dlibunwind=disabled",
        "-Dbuild-tests=false", "-Dvideo-codecs=", "-Dintel-rt=disabled")
    if (Test-Path $build) { $setup += "--wipe" }
    & meson @setup
    if ($LASTEXITCODE) { throw "meson setup failed" }
}

# --- Build ----------------------------------------------------------------------------------
& ninja -C $build src/microsoft/spirv_to_dxil/spirv_to_dxil.dll
if ($LASTEXITCODE) { throw "building spirv_to_dxil.dll failed" }

# A UWP DLL may only import what the AppContainer allows: catch a stray Win32 import here rather
# than as a load failure on-console.
$dll = Join-Path $build "src\microsoft\spirv_to_dxil\spirv_to_dxil.dll"
$imports = & dumpbin /nologo /dependents $dll | Where-Object { $_ -match "^\s+\S+\.dll$" } | ForEach-Object { $_.Trim() }
Write-Host "imports  : $($imports -join ', ')"
Write-Host "built    : $dll"

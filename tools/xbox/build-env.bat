@echo off
REM SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
REM SPDX-License-Identifier: GPL-3.0-or-later
REM
REM Sets up a UWP/Store-CRT build environment for the uwp-x64 preset, then runs whatever you pass
REM (or leaves an interactive shell if you pass nothing). Run from cmd or PowerShell, NOT Git Bash.
REM
REM   tools\xbox\build-env.bat cmake --preset uwp-x64
REM   tools\xbox\build-env.bat cmake --build --preset uwp-x64 --target eden-uwp
REM
REM The three-step dance in docs/uwp_build.md is easy to get subtly wrong and fails SILENTLY (you
REM get a desktop, non-Store-CRT build that compiles and then behaves oddly on-console), so this
REM script does it in one place and verifies the result before handing control over.

REM vswhere must be on PATH: vcvarsall shells out to it bare, and without it quietly falls back to
REM a desktop x64 environment with no Store CRT.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"

for /f "usebackq tokens=*" %%I in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo ERROR: no Visual Studio instance with the C++ toolset was found.
    exit /b 1
)
echo VS: %VSROOT%

REM English compiler messages: Ninja finds header dependencies by matching the /showIncludes
REM prefix byte for byte, and a localized prefix (e.g. Spanish) that drifts from the one CMake
REM cached silently stops header edits from triggering rebuilds. Needs the VS English language pack.
set VSLANG=1033

call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 uwp
if errorlevel 1 ( echo ERROR: vcvarsall x64 uwp failed. & exit /b 1 )

REM VS's CMake + Ninja must come first; a stray CMake elsewhere on PATH (Strawberry Perl bundles
REM one) would otherwise win.
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"

REM Native perl + nasm for the from-source OpenSSL, and glslangValidator for video_core's host
REM shaders. Prepended only if present, so a working PATH is never clobbered.
if exist "C:\Strawberry\perl\bin\perl.exe"        set "PATH=C:\Strawberry\perl\bin;%PATH%"
if exist "C:\Program Files\NASM\nasm.exe"         set "PATH=C:\Program Files\NASM;%PATH%"
if exist "C:\glslang\bin\glslangValidator.exe"    set "PATH=C:\glslang\bin;%PATH%"

REM Verify rather than assume: this is the check that catches a silent desktop fallback.
echo %LIB% | findstr /C:"x64\store" >nul
if errorlevel 1 (
    echo ERROR: LIB does not point at the Store CRT - this is NOT a UWP environment.
    echo        Is the "C++ ^(v143^) Universal Windows Platform tools" component installed?
    exit /b 1
)
echo Store CRT: OK  ^(app_plat=%VSCMD_ARG_app_plat%^)

for %%T in (cmake.exe ninja.exe cl.exe perl.exe nasm.exe glslangValidator.exe) do (
    set "FOUND="
    for %%F in ("%%~$PATH:T") do if not "%%~F"=="" set "FOUND=%%~F"
    if defined FOUND (echo   %%T: ok) else (echo   %%T: MISSING)
)

if "%~1"=="" (
    echo.
    echo Environment ready. Starting a shell - exit to return.
    cmd /k
) else (
    echo.
    %*
)

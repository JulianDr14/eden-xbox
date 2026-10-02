@echo off
REM SPDX-FileCopyrightText: Copyright 2026 JulianDr14
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Desktop SDL frontend, same fork/core, separate from the Store-CRT Xbox build.
setlocal
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq tokens=*" %%I in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "EDEN_DESKTOP_VS=%%I"
if not defined EDEN_DESKTOP_VS exit /b 1
call "%EDEN_DESKTOP_VS%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
set VSLANG=1033
REM Keep MSYS/devkitPro CMake out of this native MSVC build, including child processes.
set "EDEN_DESKTOP_CMAKE=%EDEN_DESKTOP_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "EDEN_DESKTOP_NINJA=%EDEN_DESKTOP_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%EDEN_DESKTOP_CMAKE%" (
    echo ERROR: Install the Visual Studio C++ CMake tools component.
    exit /b 1
)
if not exist "%EDEN_DESKTOP_NINJA%" (
    echo ERROR: Ninja was not found in the Visual Studio CMake tools component.
    exit /b 1
)
REM CPM needs GNU patch from Git for Windows. The devkitPro Git points it at
REM an incomplete MSYS installation, so select both tools explicitly.
set "EDEN_DESKTOP_GIT_ROOT=%ProgramFiles%\Git"
if not exist "%EDEN_DESKTOP_GIT_ROOT%\cmd\git.exe" set "EDEN_DESKTOP_GIT_ROOT=%LocalAppData%\Programs\Git"
if not exist "%EDEN_DESKTOP_GIT_ROOT%\cmd\git.exe" (
    echo ERROR: Git for Windows was not found in Program Files or LocalAppData.
    exit /b 1
)
if not exist "%EDEN_DESKTOP_GIT_ROOT%\usr\bin\patch.exe" (
    echo ERROR: GNU patch is missing from the Git for Windows installation.
    exit /b 1
)
set "PATH=%EDEN_DESKTOP_GIT_ROOT%\cmd;%PATH%"
if exist "C:\Strawberry\perl\bin\perl.exe" set "PATH=C:\Strawberry\perl\bin;%PATH%"
if exist "C:\Program Files\NASM\nasm.exe" set "PATH=C:\Program Files\NASM;%PATH%"
if exist "C:\glslang\bin\glslangValidator.exe" set "PATH=C:\glslang\bin;%PATH%"
set "PATH=%EDEN_DESKTOP_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%EDEN_DESKTOP_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cd /d "%~dp0..\.."
REM Regenerate only the configuration cache if an earlier attempt used MSYS paths.
set "EDEN_DESKTOP_FRESH="
if exist build-vulkan-pc\CMakeCache.txt (
    "%SystemRoot%\System32\findstr.exe" /L /C:"msys2" /C:"/home/" /C:"/opt/devkitpro" build-vulkan-pc\CMakeCache.txt >nul
    if not errorlevel 1 (
        echo Regenerating the configuration created by MSYS/devkitPro CMake.
        set "EDEN_DESKTOP_FRESH=--fresh"
    )
)
"%EDEN_DESKTOP_CMAKE%" %EDEN_DESKTOP_FRESH% -S . -B build-vulkan-pc -G Ninja "-DCMAKE_MAKE_PROGRAM=%EDEN_DESKTOP_NINJA%" "-DGIT_EXECUTABLE=%EDEN_DESKTOP_GIT_ROOT%\cmd\git.exe" "-DPATCH_EXE=%EDEN_DESKTOP_GIT_ROOT%\usr\bin\patch.exe" -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=OFF -DYUZU_CMD=ON -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DENABLE_OPENGL=OFF -DENABLE_D3D12=OFF -DENABLE_RESHADE=OFF -DENABLE_CUBEB=OFF -DENABLE_LIBUSB=OFF -DENABLE_WEB_SERVICE=OFF -DUSE_DISCORD_PRESENCE=OFF -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON -DDYNARMIC_UWP_APPCONTAINER=OFF -DGLSLANGVALIDATOR=C:/glslang/bin/glslangValidator.exe
if not "%errorlevel%"=="0" exit /b %errorlevel%
"%EDEN_DESKTOP_CMAKE%" --build build-vulkan-pc --target yuzu-cmd --parallel 4
if not "%errorlevel%"=="0" exit /b %errorlevel%
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File tools\xbox\vulkan-run.ps1
exit /b %errorlevel%

@echo off
REM SPDX-FileCopyrightText: Copyright 2026 JulianDr14
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Run from the repository root. Desktop storage gates, not a console certificate.
setlocal
if not exist build-uwp\CMakeCache.txt exit /b 1
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PULSE_VS=%%I"
if not defined PULSE_VS exit /b 1
for /f "tokens=2 delims==" %%I in ('findstr /b "CPM_PACKAGE_lz4_SOURCE_DIR:INTERNAL=" build-uwp\CMakeCache.txt') do set "PULSE_LZ4=%%I"
if not defined PULSE_LZ4 exit /b 1
call "%PULSE_VS%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 exit /b 1
if not exist build-uwp\pulse-gate mkdir build-uwp\pulse-gate
cl /nologo /O2 /W4 /WX /c "%PULSE_LZ4%\lib\lz4.c" /Fo:build-uwp\pulse-gate\lz4.obj
if errorlevel 1 exit /b 1
cl /nologo /O2 /std:c++20 /EHsc /W4 /WX /I src /I "%PULSE_LZ4%\lib" tools\xbox\tests\pulse-archive.cpp src\common\pulse_archive.cpp build-uwp\pulse-gate\lz4.obj /Fo:build-uwp\pulse-gate\ /Fe:build-uwp\pulse-gate\pulse-archive.exe
if errorlevel 1 exit /b 1
build-uwp\pulse-gate\pulse-archive.exe
if errorlevel 1 exit /b 1
cl /nologo /O2 /std:c++20 /EHsc /W4 /WX /I src /I "%PULSE_LZ4%\lib" tools\xbox\tests\pulse-store.cpp src\common\pulse_archive.cpp src\common\pulse_store.cpp build-uwp\pulse-gate\lz4.obj /Fo:build-uwp\pulse-gate\ /Fe:build-uwp\pulse-gate\pulse-store.exe /link WindowsApp.lib
if errorlevel 1 exit /b 1
build-uwp\pulse-gate\pulse-store.exe
exit /b %errorlevel%

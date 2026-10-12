# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from the repo root after building the changed UWP objects. These are component gates;
# they do not certify DSP/GPU/kernel restoration or a persistent homebrew session.
$ErrorActionPreference = 'Stop'
$pulseVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$pulseVs) { throw 'MSVC not found' }
$pulseBoost = Get-ChildItem '.cache/cpm/boost' -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$pulseBoost) { throw 'Bundled Boost not found' }
$pulseIncludes = Get-ChildItem "$($pulseBoost.FullName)/libs" -Directory | ForEach-Object { '/I "' + $_.FullName + '\include"' }
$pulseFmt = Get-Content 'build-uwp/CMakeCache.txt' | Where-Object { $_.StartsWith('CPM_PACKAGE_fmt_SOURCE_DIR:INTERNAL=') } | Select-Object -First 1
if (!$pulseFmt) { throw 'Configured fmt source missing' }
$pulseFmt = $pulseFmt.Substring($pulseFmt.IndexOf('=') + 1)
$pulseGate = New-Item -ItemType Directory -Force 'build-uwp/pulse-gate'
$pulseFlags = '/nologo /O2 /MD /std:c++20 /utf-8 /EHsc /DNOMINMAX /I src /I src/dynarmic/src /I "' + $pulseFmt + '/include" ' + ($pulseIncludes -join ' ')
$pulseCommon = 'build-uwp\src\common\common.lib build-uwp\_deps\fmt-build\fmt.lib'
$pulseTests = @(
    @{ Name = 'pulse-exclusive'; Source = 'pulse-exclusive'; Objects = 'src\dynarmic\src\dynarmic\backend\x64\exclusive_monitor.cpp build-uwp\src\dynarmic\src\dynarmic\CMakeFiles\dynarmic.dir\common\spin_lock_x64.cpp.obj'; Libraries = "$pulseCommon WindowsApp.lib ntdll.lib" },
    @{ Name = 'pulse-wait'; Source = 'pulse-wait'; Objects = ''; Libraries = '' },
    @{ Name = 'pulse-process'; Source = 'pulse-process'; Objects = ''; Libraries = '' },
    @{ Name = 'pulse-machine'; Source = 'pulse-machine'; Objects = ''; Libraries = "$pulseCommon build-uwp\_deps\lz4-build\lz4.lib WindowsApp.lib ntdll.lib" },
    @{ Name = 'pulse-readback'; Source = 'gc-readback'; Objects = ''; Libraries = '' },
    @{ Name = 'pulse-services'; Source = 'pulse-services'; Objects = ''; Libraries = '' },
    @{ Name = 'pulse-filesystem'; Source = 'pulse-filesystem'; Objects = 'build-uwp\src\core\CMakeFiles\core.dir\file_sys\vfs\vfs.cpp.obj build-uwp\src\core\CMakeFiles\core.dir\file_sys\vfs\vfs_real.cpp.obj build-uwp\src\core\CMakeFiles\core.dir\file_sys\vfs\vfs_offset.cpp.obj'; Libraries = "$pulseCommon WindowsApp.lib ntdll.lib" },
    @{ Name = 'pulse-audio'; Source = 'pulse-audio'; Objects = 'build-uwp\src\core\CMakeFiles\core.dir\core_timing.cpp.obj build-uwp\src\audio_core\CMakeFiles\audio_core.dir\sink\sink_stream.cpp.obj build-uwp\src\audio_core\CMakeFiles\audio_core.dir\sink\xaudio2_sink.cpp.obj'; Libraries = "$pulseCommon WindowsApp.lib ntdll.lib xaudio2.lib" }
)
foreach ($pulseTest in $pulseTests) {
    $pulseCompile = 'cl ' + $pulseFlags + ' tools\xbox\tests\' + $pulseTest.Source + '.cpp ' + $pulseTest.Objects + ' /Fo:build-uwp\pulse-gate\ /Fe:build-uwp\pulse-gate\' + $pulseTest.Name + '.exe /link /OPT:REF ' + $pulseTest.Libraries
    $pulseCommands = @('@echo off', ('call "' + $pulseVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1', $pulseCompile)
    Set-Content "$($pulseGate.FullName)/phase3.cmd" -Value $pulseCommands -Encoding ascii
    & "$env:SystemRoot\System32\cmd.exe" /d /c "$($pulseGate.FullName)\phase3.cmd"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $($pulseTest.Name)" }
    & "$($pulseGate.FullName)/$($pulseTest.Name).exe"
    if ($LASTEXITCODE -ne 0) { throw "Gate failed: $($pulseTest.Name)" }
}

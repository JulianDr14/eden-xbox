# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
param([string[]]$Tests = @('frontend-input', 'controller-selection', 'stick-navigation',
    'game-menu', 'game-keyboard', 'system-settings', 'cheats-page', 'mods-page',
    'text-entry', 'localization-catalogs'))
$ErrorActionPreference = 'Stop'
$frontendOutput = New-Item -ItemType Directory -Force build-uwp/test-results/frontend-refactor
$frontendVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$frontendVs) { throw 'MSVC not found' }
$frontendCommands = @('@echo off', ('call "' + $frontendVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1')
foreach ($frontendTest in $Tests) {
    if ($frontendTest -notmatch '^[a-z0-9-]+$' -or !(Test-Path "tools/xbox/tests/$frontendTest.cpp")) {
        throw "Unknown frontend test: $frontendTest"
    }
    $frontendExtra = if ($frontendTest -eq 'cheats-page') { 'src\core\memory\cheat_text.cpp ' } else { '' }
    $frontendCommands += ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /DEDEN_XBOX_NO_WINRT /I src tools\xbox\tests\' + $frontendTest + '.cpp ' + $frontendExtra + 'src\eden_uwp\localization.cpp src\eden_uwp\locales\en.cpp src\eden_uwp\locales\es.cpp src\eden_uwp\locales\registry.cpp /Fo:build-uwp\test-results\frontend-refactor\ /Fe:build-uwp\test-results\frontend-refactor\' + $frontendTest + '.exe')
    $frontendCommands += 'if errorlevel 1 exit /b 1'
    $frontendCommands += ('build-uwp\test-results\frontend-refactor\' + $frontendTest + '.exe')
    $frontendCommands += 'if errorlevel 1 exit /b 1'
}
Set-Content "$($frontendOutput.FullName)/build.cmd" $frontendCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($frontendOutput.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Frontend gate failed' }

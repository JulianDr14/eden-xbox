# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Capture thread stacks/metadata of a live, stuck PC session. No full guest memory dump.
param([int] $TargetProcessId = 0,
      [string] $OutputDirectory = (Join-Path $env:LOCALAPPDATA 'EdenXbox/HangDumps'))
$ErrorActionPreference = 'Stop'
if (-not $TargetProcessId) {
    $hangTargets = @(Get-Process -Name eden-uwp -ErrorAction Stop)
    if ($hangTargets.Count -ne 1) { throw 'Specify -TargetProcessId for the intended session' }
    $TargetProcessId = $hangTargets[0].Id
}
$hangProcess = Get-Process -Id $TargetProcessId -ErrorAction Stop
if (-not ('EdenHangDump' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class EdenHangDump {
    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool MiniDumpWriteDump(IntPtr process, uint processId,
        IntPtr file, uint type, IntPtr exception, IntPtr streams, IntPtr callback);
}
'@
}
$hangDirectory = New-Item -ItemType Directory -Force -Path $OutputDirectory
$hangStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
for ($hangIndex = 1; $hangIndex -le 2; ++$hangIndex) {
    $hangPath = Join-Path $hangDirectory.FullName "$($hangProcess.ProcessName)-$TargetProcessId-$hangStamp-$hangIndex.dmp"
    $hangFile = [IO.File]::Open($hangPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite,
                               [IO.FileShare]::None)
    try {
        # Normal dump + full memory *metadata* + thread metadata, not full memory contents.
        if (-not [EdenHangDump]::MiniDumpWriteDump($hangProcess.Handle, $TargetProcessId,
            $hangFile.SafeFileHandle.DangerousGetHandle(), 0x1800,
            [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)) {
            throw (New-Object ComponentModel.Win32Exception([Runtime.InteropServices.Marshal]::GetLastWin32Error()))
        }
    } finally { $hangFile.Dispose() }
    Get-Item -LiteralPath $hangPath | Select-Object FullName, Length
    if ($hangIndex -eq 1) { Start-Sleep -Seconds 1 }
}

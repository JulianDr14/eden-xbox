# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
# Creates a >4GiB NTFS sparse fixture in the app's test folder, then exercises
# both UWP readers and persisted permission across two process launches.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pkg = Get-AppxPackage -Name EdenEmuProject.EdenXbox
if (-not $pkg) { throw 'Register the local app with local-run.ps1 first.' }
if (Get-Process eden-uwp -ErrorAction SilentlyContinue) { throw 'Close the app before running this gate.' }
$gateDir = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState\rom-storage-gate"
New-Item -ItemType Directory -Path $gateDir -Force | Out-Null
$fixture = Join-Path $gateDir 'fixture.bin'
if (Test-Path -LiteralPath $fixture) { throw "Existing gate fixture: $fixture. Remove it after checking its origin." }
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class RomStorageFixture {
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool DeviceIoControl(SafeFileHandle file, uint code,
        IntPtr input, uint inputSize, IntPtr output, uint outputSize,
        out uint returned, IntPtr overlapped);
    private static void Region(FileStream file, long offset, int length) {
        var bytes = new byte[length];
        for (int i=0; i<length; ++i) {
            ulong p=(ulong)(offset+i);
            bytes[i]=(byte)((p*17) ^ (p>>16) ^ 0x5a);
        }
        file.Position=offset;
        file.Write(bytes,0,bytes.Length);
    }
    public static void Create(string path) {
        using (var file=new FileStream(path,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.Read)) {
            uint returned;
            // FSCTL_SET_SPARSE: never physically allocate four gigabytes for this test.
            if (!DeviceIoControl(file.SafeFileHandle,0x900c4,IntPtr.Zero,0,
                                 IntPtr.Zero,0,out returned,IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            long size=(1L<<32)+2*1024*1024+257;
            file.SetLength(size);
            Region(file,0,2*1024*1024+257);
            Region(file,(1L<<32)-97,512);
            Region(file,size-64,64);
        }
    }
}
'@
[RomStorageFixture]::Create($fixture)
for ($i = 0; $i -lt 70; ++$i) {
    [IO.File]::WriteAllBytes((Join-Path $gateDir "page-$i.nro"), [byte[]]@())
}
foreach ($name in @('a', 'b')) {
    $nested = Join-Path $gateDir $name
    New-Item -ItemType Directory -Path $nested -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'tools\xbox\boot_nro\boot.nro') -Destination (Join-Path $nested 'same.nro')
}
Write-Host "Sparse fixture: $fixture (4 GiB + 2 MiB + 257 bytes logical; ~2 MiB written)"
$output = Join-Path $repo 'build-uwp\rom-storage-gate'
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach ($phase in @('record', 'restore')) {
    & (Join-Path $PSScriptRoot 'local-run.ps1') -NoBuild -TimeoutSec 75 -BootCfg "rom_storage_checks=$phase" |
        Tee-Object -FilePath (Join-Path $output "$phase-launcher.txt")
    $local = Split-Path -Parent $gateDir
    $diag = Join-Path $local 'eden_uwp_diag.txt'
    Copy-Item -LiteralPath $diag -Destination (Join-Path $output "$phase-diag.txt")
    $log = Join-Path $local 'eden\log\eden_log.txt'
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $output "$phase-log.txt") }
    $text = Get-Content -LiteralPath $diag -Raw
    if ($text -notmatch "ROM storage gate PASS phase=$phase" -or $text -notmatch 'RunHeadlessBoot returned 0') {
        throw "ROM storage gate failed in $phase; evidence saved under $output"
    }
}
# Delete only the exact fixture created by this script; keep evidence and the folder.
Remove-Item -LiteralPath $fixture
for ($i = 0; $i -lt 70; ++$i) { Remove-Item -LiteralPath (Join-Path $gateDir "page-$i.nro") }
foreach ($name in @('a', 'b')) { Remove-Item -LiteralPath (Join-Path $gateDir "$name\same.nro") }
Write-Host "PASS: direct + WinRT, >4GiB/EOF, concurrent reads, token restored after process restart. Evidence: $output"

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stages, packs and signs the Phase-2 headless boot appx (src/eden_uwp) for sideloading
# onto an Xbox Series X|S in Dev Mode. See docs/xbox_deploy.md for the full flow.
#
#   .\tools\xbox\package-appx.ps1 -BootNro C:\path\to\boot.nro
#
# Produces build-uwp\package\eden-xbox.appx plus the .cer you must install on the console.

[CmdletBinding()]
param(
    # Directory the uwp-x64 preset built into (CMakePresets.json binaryDir).
    [string] $BuildDir = "build-uwp",
    # Homebrew NRO bundled as the GATE-2 payload. It must emit the JIT-liveness sentinel
    # EDEN_XBOX_JIT_ALIVE via svcOutputDebugString. NO keys/firmware/commercial ROMs (house rule).
    [string] $BootNro,
    # Must match Identity/@Publisher in dist/uwp/AppxManifest.xml, character for character.
    [string] $PublisherCN = "CN=EdenXboxDev",
    [string] $OutDir = "build-uwp\package"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo

function Find-SdkTool([string] $name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "${env:ProgramFiles}\Windows Kits\10\bin")
    $hit = $roots | Where-Object { Test-Path $_ } | ForEach-Object {
        Get-ChildItem $_ -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -like '*\x64' }
    } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $hit) { throw "$name not found. Install the Windows 10/11 SDK (it ships MakeAppx + SignTool)." }
    return $hit.FullName
}

# --- 1. locate the built exe -------------------------------------------------------------
$exe = Join-Path $repo "$BuildDir\bin\eden-uwp.exe"
if (-not (Test-Path $exe)) {
    $found = Get-ChildItem (Join-Path $repo $BuildDir) -Recurse -Filter eden-uwp.exe -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) {
        throw "eden-uwp.exe not found under $BuildDir. Build it first from a 'vcvarsall.bat x64 uwp' shell:`n" +
              "  cmake --preset uwp-x64`n  cmake --build --preset uwp-x64 --target eden-uwp"
    }
    $exe = $found.FullName
}
Write-Host "exe      : $exe"

# --- 2. stage the package layout ---------------------------------------------------------
$layout = Join-Path $repo "$OutDir\layout"
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Path $layout -Force | Out-Null

Copy-Item $exe $layout
Copy-Item (Join-Path $repo "dist\uwp\AppxManifest.xml") $layout
Copy-Item (Join-Path $repo "dist\uwp\Assets") $layout -Recurse

# Any runtime DLLs the link produced land next to the exe; carry them along.
Get-ChildItem (Split-Path $exe) -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName $layout }

# uwp_boot.cpp reads Package.InstalledLocation\boot.nro - the payload must sit at the layout root.
if ($BootNro) {
    if (-not (Test-Path $BootNro)) { throw "BootNro not found: $BootNro" }
    Copy-Item $BootNro (Join-Path $layout "boot.nro")
    Write-Host "payload  : $BootNro -> boot.nro"
} else {
    Write-Warning "No -BootNro given. The app will activate, fail to load the NRO and log status 2 to eden_uwp_diag.txt."
}

# Publisher mismatch between manifest and cert is the #1 sideload rejection; fail loudly here.
[xml]$mf = Get-Content (Join-Path $layout "AppxManifest.xml") -Raw
if ($mf.Package.Identity.Publisher -ne $PublisherCN) {
    throw "Publisher mismatch: manifest has '$($mf.Package.Identity.Publisher)' but signing with '$PublisherCN'."
}

# --- 3. pack -----------------------------------------------------------------------------
$makeappx = Find-SdkTool "MakeAppx.exe"
$appx = Join-Path $repo "$OutDir\eden-xbox.appx"
if (Test-Path $appx) { Remove-Item $appx -Force }
& $makeappx pack /d $layout /p $appx /o
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)." }

# --- 4. sign -----------------------------------------------------------------------------
# Reuse a matching cert if one is already in the user store, else mint a self-signed one.
$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $PublisherCN -and $_.NotAfter -gt (Get-Date) } |
        Select-Object -First 1
if (-not $cert) {
    Write-Host "minting a self-signed code-signing cert for $PublisherCN"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $PublisherCN `
        -KeyUsage DigitalSignature -FriendlyName "Eden Xbox sideload" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
}

$signtool = Find-SdkTool "SignTool.exe"
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com $appx
if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)." }

# The console must trust the signer: upload this .cer alongside the appx in the Device Portal.
$cer = Join-Path $repo "$OutDir\eden-xbox.cer"
Export-Certificate -Cert $cert -FilePath $cer -Type CERT | Out-Null

# --- 5. the VCLibs framework package ------------------------------------------------------
# The exe hard-imports the Store CRT, which lives in the Microsoft.VCLibs.140.00 framework
# package rather than ours (see the PackageDependency in the manifest). The console needs it
# installed too, so hand it over next to our package instead of leaving the user to find it.
# It ships with the VS "C++ (v143) UWP tools" component, as an Extension SDK.
$vclibs = $null
foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
    if (-not $pf) { continue }
    $cand = Join-Path $pf "Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx"
    if (Test-Path -LiteralPath $cand) { $vclibs = $cand; break }
}
if ($vclibs) {
    $vcOut = Join-Path $repo "$OutDir\Microsoft.VCLibs.x64.14.00.appx"
    Copy-Item -LiteralPath $vclibs -Destination $vcOut -Force
} else {
    Write-Warning ("Microsoft.VCLibs.x64.14.00.appx not found. The app will FAIL TO ACTIVATE on-console " +
                   "without it. Install the VS component Microsoft.VisualStudio.ComponentGroup.UWP.VC.")
}

Write-Host ""
Write-Host "package  : $appx"
Write-Host "cert     : $cer"
if ($vclibs) { Write-Host "framework: $vcOut  (upload as a dependency package)" }
Write-Host "Next: Device Portal https://<xbox-ip>:11443 -> Add -> upload all of the above -> set the app to Game mode."

<#
Builds the Release app and the Inno Setup installer, signing everything Orbit ships under
its own name when a code-signing certificate is configured (see sign.ps1). Third-party
tools bundled next to Orbit (yt-dlp, ffmpeg, Qt, OpenSSL) keep their own signatures.

  powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 [-SkipBuild] [-RequireSigning]

Prints the installer's size and SHA-256, which go into the website's release.js.
#>
param(
    [switch]$SkipBuild,
    [switch]$RequireSigning
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sign = Join-Path $PSScriptRoot 'sign.ps1'
$release = Join-Path $root 'build-vcpkg\Release'

$versionLine = Select-String -Path (Join-Path $PSScriptRoot 'Orbit.iss') -Pattern '^#define AppVersion "([^"]+)"' | Select-Object -First 1
if (-not $versionLine) { throw 'AppVersion was not found in Orbit.iss' }
$version = $versionLine.Matches[0].Groups[1].Value

if (-not $SkipBuild) {
    Write-Host "Building Orbit $version..."
    & cmake --build (Join-Path $root 'build-vcpkg') --target orbit --config Release
    if ($LASTEXITCODE -ne 0) { throw 'The build failed' }
}

$signing = [bool]($env:ORBIT_SIGN_THUMBPRINT -or $env:ORBIT_SIGN_PFX -or ($env:ORBIT_SIGN_AZURE_DLIB -and $env:ORBIT_SIGN_AZURE_METADATA))
if (-not $signing -and $RequireSigning) { throw 'Signing was required, but no certificate is configured (see installer\sign.ps1)' }

if ($signing) {
    foreach ($exe in @('orbit.exe', 'orbit-native-host.exe')) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $sign (Join-Path $release $exe)
        if ($LASTEXITCODE -ne 0) { throw "Signing $exe failed" }
    }
} else {
    Write-Warning 'Code signing is not configured: the installer will be unsigned and Windows SmartScreen will warn about it.'
}

$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'ISCC.exe (Inno Setup 6) was not found' }

$iss = Join-Path $PSScriptRoot 'Orbit.iss'
if ($signing) {
    $signCommand = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `$q$sign`$q `$f"
    & $iscc /Q /DSignSetup "/Sorbitsign=$signCommand" $iss
} else {
    & $iscc /Q $iss
}
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }

$installer = Join-Path $PSScriptRoot "Output\OrbitSetup-$version.exe"
$file = Get-Item $installer
$hash = (Get-FileHash $installer -Algorithm SHA256).Hash
$signature = Get-AuthenticodeSignature $installer

Write-Host ''
Write-Host "Installer : $installer"
Write-Host ("Size      : {0:N0} bytes ({1:N1} MB)" -f $file.Length, ($file.Length / 1MB))
Write-Host "SHA-256   : $hash"
Write-Host ("Signature : {0}{1}" -f $signature.Status, $(if ($signature.SignerCertificate) { " - " + $signature.SignerCertificate.Subject } else { '' }))

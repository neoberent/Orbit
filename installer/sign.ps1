<#
Signs one file with Authenticode (SHA-256, RFC 3161 timestamp).

Configure exactly one certificate source through environment variables:

  Certificate in the Windows certificate store (hardware tokens, Certum SimplySign and
  other cloud HSMs show up here):
    ORBIT_SIGN_THUMBPRINT     SHA-1 thumbprint of the code-signing certificate

  Certificate file:
    ORBIT_SIGN_PFX            path to a .pfx/.p12 file
    ORBIT_SIGN_PFX_PASSWORD   its password (optional)

  Azure Artifact Signing (formerly Trusted Signing):
    ORBIT_SIGN_AZURE_DLIB     path to Azure.CodeSigning.Dlib.dll
    ORBIT_SIGN_AZURE_METADATA path to the metadata.json naming the account and profile

Optional:
    ORBIT_SIGNTOOL            full path to signtool.exe (found in the Windows SDK otherwise)
    ORBIT_SIGN_TIMESTAMP      timestamp server URL
    ORBIT_SIGN_ALLOW_UNTRUSTED=1  accept a certificate Windows doesn't trust (testing only)

Exit codes: 0 signed, 2 not configured, 1 failed.
#>
param([Parameter(Mandatory = $true)][string]$Path)

$ErrorActionPreference = 'Stop'

function Find-SignTool {
    if ($env:ORBIT_SIGNTOOL -and (Test-Path $env:ORBIT_SIGNTOOL)) { return $env:ORBIT_SIGNTOOL }
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $candidate = Get-ChildItem $root -Directory -Filter '10.*' |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }
    $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

if (-not (Test-Path $Path)) { Write-Error "File not found: $Path"; exit 1 }

$signArgs = @('sign', '/fd', 'SHA256', '/td', 'SHA256', '/d', 'Orbit Download Manager', '/du', 'https://orbitdm.netlify.app')
$timestamp = $env:ORBIT_SIGN_TIMESTAMP

if ($env:ORBIT_SIGN_AZURE_DLIB -and $env:ORBIT_SIGN_AZURE_METADATA) {
    if (-not $timestamp) { $timestamp = 'http://timestamp.acs.microsoft.com' }
    $signArgs += @('/dlib', $env:ORBIT_SIGN_AZURE_DLIB, '/dmdf', $env:ORBIT_SIGN_AZURE_METADATA)
} elseif ($env:ORBIT_SIGN_PFX) {
    $signArgs += @('/f', $env:ORBIT_SIGN_PFX)
    if ($env:ORBIT_SIGN_PFX_PASSWORD) { $signArgs += @('/p', $env:ORBIT_SIGN_PFX_PASSWORD) }
} elseif ($env:ORBIT_SIGN_THUMBPRINT) {
    $signArgs += @('/sha1', ($env:ORBIT_SIGN_THUMBPRINT -replace '\s', ''))
} else {
    Write-Host 'Code signing is not configured (see installer\sign.ps1).'
    exit 2
}
if (-not $timestamp) { $timestamp = 'http://timestamp.digicert.com' }
$signArgs += @('/tr', $timestamp)

$signtool = Find-SignTool
if (-not $signtool) { Write-Error 'signtool.exe was not found. Install the Windows SDK or set ORBIT_SIGNTOOL.'; exit 1 }

# Timestamp servers are occasionally unavailable, so retry a few times before giving up.
$signed = $false
for ($attempt = 1; $attempt -le 3 -and -not $signed; $attempt++) {
    & $signtool @signArgs $Path | Out-Host
    if ($LASTEXITCODE -eq 0) { $signed = $true } else { Start-Sleep -Seconds (5 * $attempt) }
}
if (-not $signed) { Write-Error "Signing failed: $Path"; exit 1 }

& $signtool verify /pa /q $Path | Out-Host
if ($LASTEXITCODE -ne 0) {
    if ($env:ORBIT_SIGN_ALLOW_UNTRUSTED -eq '1') {
        Write-Warning "Signed, but Windows doesn't trust the certificate (allowed for testing): $Path"
    } else {
        Write-Error "The signature on $Path doesn't verify. Is the certificate trusted and valid for code signing?"
        exit 1
    }
}
Write-Host "Signed $Path"
exit 0

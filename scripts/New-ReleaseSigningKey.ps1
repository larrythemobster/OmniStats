param(
    [string]$KeyPath = (Join-Path $PSScriptRoot "..\..\certs\release-signing.key"),
    [string]$SignerPath = ""
)

$ErrorActionPreference = "Stop"

if (-not $SignerPath) {
    $buildDir = Join-Path $PSScriptRoot "..\out\windows-release"
    $found = Get-ChildItem -Path $buildDir -Recurse -Filter OmniStatsReleaseSigner.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { throw "OmniStatsReleaseSigner.exe not found under $buildDir. Build the windows-release preset first or pass -SignerPath." }
    $SignerPath = $found.FullName
}

if (Test-Path $KeyPath) {
    $bytes = [System.IO.File]::ReadAllBytes($KeyPath)
    if ($bytes.Length -ne 64) { throw "Existing key at $KeyPath has unexpected length $($bytes.Length) bytes." }
    # libsodium secret keys are seed || public key.
    $pubHex = -join ($bytes[32..63] | ForEach-Object { $_.ToString("x2") })
    Write-Host "Release signing key already exists at: $KeyPath"
    Write-Host "CURRENT_RELEASE_PUBLIC_KEY_HEX = `"$pubHex`""
    return
}

$certDir = Split-Path -Parent $KeyPath
if (-not (Test-Path $certDir)) { New-Item -ItemType Directory -Force -Path $certDir | Out-Null }

& $SignerPath generate-key $KeyPath
if ($LASTEXITCODE -ne 0) { throw "OmniStatsReleaseSigner generate-key failed." }

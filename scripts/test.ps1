[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipImportAudit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset = if ($Configuration -eq 'Release') { 'mingw64-release' } else { 'mingw64-dev' }
$binDir = Join-Path $root "out/build/$preset/bin"

Push-Location $root
try {
    & ctest --preset $preset
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed with code $LASTEXITCODE"
    }

    if (-not $SkipImportAudit) {
        & (Join-Path $PSScriptRoot 'audit-pe-imports.ps1') -Binary @(
            (Join-Path $binDir 'mosh.exe'),
            (Join-Path $binDir 'mosh-client.exe'))
        if ($LASTEXITCODE -ne 0) {
            throw "PE import audit failed with code $LASTEXITCODE"
        }
    }
} finally {
    Pop-Location
}

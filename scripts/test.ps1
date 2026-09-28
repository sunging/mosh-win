<#
.SYNOPSIS
Runs CTest for a configured preset and audits the shipped executables' PE
imports.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipImportAudit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MoshWin.psm1') -Force

$root = Get-MoshWinRoot
$preset = Get-MoshWinPreset $Configuration
$binDir = Join-Path $root "out/build/$preset/bin"

Push-Location $root
try {
    Invoke-Native 'ctest' @('--preset', $preset)

    if (-not $SkipImportAudit) {
        & (Join-Path $PSScriptRoot 'audit-pe-imports.ps1') -Binary @(
            (Join-Path $binDir 'mosh.exe'),
            (Join-Path $binDir 'mosh-client.exe'))
    }
} finally {
    Pop-Location
}

<#
.SYNOPSIS
Builds the release twice in two clean directories and checks that mosh.exe
and mosh-client.exe are bit-identical.

.PARAMETER DependencySourceRoot
Build offline from these dependency trees instead of downloading them.
#>

# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$DependencySourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MoshWin.psm1') -Force

$root = Get-MoshWinRoot
$primary = Join-Path $root "out/build/$(Get-MoshWinPreset Release)"
$secondary = Join-Path $root 'out/build/mingw64-repro'

$common = @{
    Configuration = 'Release'
    Clean = $true
}
if ($DependencySourceRoot) {
    $common.DependencySourceRoot = Resolve-MoshWinPath $DependencySourceRoot
    $common.Offline = $true
}

& (Join-Path $PSScriptRoot 'build.ps1') @common -BuildDirectory $primary
& (Join-Path $PSScriptRoot 'build.ps1') @common -BuildDirectory $secondary

$mismatch = $false
$results = foreach ($name in @('mosh.exe', 'mosh-client.exe')) {
    $primaryHash = Get-Sha256 (Join-Path $primary "bin/$name")
    $secondaryHash = Get-Sha256 (Join-Path $secondary "bin/$name")
    $match = $primaryHash -ceq $secondaryHash
    if (-not $match) { $mismatch = $true }

    [pscustomobject]@{
        File = $name
        PrimarySHA256 = $primaryHash
        SecondarySHA256 = $secondaryHash
        Match = $match
    }
}

$results | Format-Table -AutoSize | Out-Host
if ($mismatch) {
    throw 'Release executable reproducibility check failed.'
}

Write-Host 'Reproducibility check passed for both release executables.'

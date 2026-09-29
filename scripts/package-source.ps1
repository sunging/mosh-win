<#
.SYNOPSIS
Writes the GPL corresponding-source package: this project plus the exact
Mosh, protobuf and zlib source trees used by the release build.

.PARAMETER BuildDir
Release build directory whose _deps/<name>-src trees are bundled.

.PARAMETER DependencySourceRoot
Bundle these dependency trees instead (for builds configured with
-DependencySourceRoot).
#>

# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$Version,
    [string]$BuildDir,
    [string]$DependencySourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MoshWin.psm1') -Force

if (-not $Version) { $Version = Get-MoshWinVersion }
$root = Get-MoshWinRoot
$BuildDir = if ($BuildDir) {
    Resolve-MoshWinPath $BuildDir
} else {
    Join-Path $root "out/build/$(Get-MoshWinPreset Release)"
}
if ($DependencySourceRoot) {
    $DependencySourceRoot = Resolve-MoshWinPath $DependencySourceRoot
}
$distDir = Join-Path $root 'dist'
$stageDir = Join-Path $distDir "mosh-win-$Version-source"
$archive = "$stageDir.zip"

Reset-Directory $stageDir
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }

Push-Location $root
try {
    # Local tool state, build output and per-user CMake presets never ship.
    Invoke-Robocopy $root (Join-Path $stageDir 'mosh-win') @(
        '/XD', '.git', 'out', 'dist', 'build', '.agents', '.cache', '.codex', '.vscode', '.idea',
        '/XF', '*.user', 'CMakeUserPresets.json')

    $thirdPartyDir = Join-Path $stageDir 'third_party/source'
    New-Item -ItemType Directory -Path $thirdPartyDir -Force | Out-Null
    foreach ($dependency in Get-MoshWinDependencyNames) {
        $source = if ($DependencySourceRoot) {
            Join-Path $DependencySourceRoot $dependency
        } else {
            Join-Path $BuildDir "_deps/$dependency-src"
        }
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "Corresponding dependency source is missing: $source. Configure the release preset first or pass -DependencySourceRoot."
        }
        Invoke-Robocopy $source (Join-Path $thirdPartyDir $dependency)
    }

    Write-Sha256Manifest -Directory $stageDir -Output (Join-Path $stageDir 'SHA256SUMS.txt')
    Compress-Archive -LiteralPath $stageDir -DestinationPath $archive -CompressionLevel Optimal
    Write-Host "Corresponding-source package: $archive"
} finally {
    Pop-Location
}

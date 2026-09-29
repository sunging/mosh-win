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
    # In a Git work tree, take the files Git sees (tracked plus untracked but
    # not ignored), so IDE build directories and other ignored output stay
    # out whatever their name.  An unpacked source package has no .git; fall
    # back to excluding the known locations there.
    $projectDestination = Join-Path $stageDir 'mosh-win'
    $gitTop = & git -C $root rev-parse --show-toplevel 2>$null
    $inGitTree = $LASTEXITCODE -eq 0 -and $gitTop -and
        [IO.Path]::GetFullPath($gitTop).TrimEnd('\') -ieq $root.TrimEnd('\')
    $global:LASTEXITCODE = 0
    if ($inGitTree) {
        $listing = & git -C $root -c core.quotePath=false ls-files -z --cached --others --exclude-standard
        if ($LASTEXITCODE -ne 0) { throw "git ls-files failed with code $LASTEXITCODE" }
        foreach ($relative in @($listing -split "`0" | Where-Object { $_ } | Sort-Object -Unique)) {
            $source = Join-Path $root $relative
            # Tracked files deleted in the work tree are not part of it.
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
            $destination = Join-Path $projectDestination $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
        }
    } else {
        Invoke-Robocopy $root $projectDestination @(
            '/XD', '.git', 'out', 'dist', 'build', 'cmake-build-*', '.agents', '.cache', '.codex',
            '.vscode', '.idea',
            '/XF', '*.user', 'CMakeUserPresets.json')
    }

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

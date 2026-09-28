<#
.SYNOPSIS
Builds the release, audits it and writes the binary and corresponding-source
ZIP packages plus SHA256SUMS.txt to dist/.

.PARAMETER Version
Package version; defaults to the version declared in CMakeLists.txt.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [switch]$SkipBuild,
    [switch]$SkipSourceBundle,
    [string]$DependencySourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MoshWin.psm1') -Force

if (-not $Version) { $Version = Get-MoshWinVersion }
$root = Get-MoshWinRoot
$buildDir = Join-Path $root "out/build/$(Get-MoshWinPreset Release)"
$binDir = Join-Path $buildDir 'bin'
$distDir = Join-Path $root 'dist'
$stageDir = Join-Path $distDir "mosh-win-$Version-x64"
$archive = "$stageDir.zip"

Push-Location $root
try {
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
    }

    $binaries = @((Join-Path $binDir 'mosh.exe'), (Join-Path $binDir 'mosh-client.exe'))
    & (Join-Path $PSScriptRoot 'audit-pe-imports.ps1') -Binary $binaries

    New-Item -ItemType Directory -Path $distDir -Force | Out-Null
    Reset-Directory $stageDir
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    Copy-Item -LiteralPath $binaries -Destination $stageDir
    Copy-Item -LiteralPath @(
        (Join-Path $root 'README.md'),
        (Join-Path $root 'LICENSE'),
        (Join-Path $root 'THIRD_PARTY_NOTICES.md')) -Destination $stageDir

    $licenseSource = Join-Path $root 'third_party/licenses'
    if (-not (Test-Path -LiteralPath $licenseSource -PathType Container)) {
        throw "Bundled runtime licenses are missing: $licenseSource"
    }
    $licenseDestination = Join-Path $stageDir 'licenses'
    New-Item -ItemType Directory -Path $licenseDestination | Out-Null
    $licenseFiles = @(Get-ChildItem -LiteralPath $licenseSource -File)
    Copy-Item -LiteralPath $licenseFiles.FullName -Destination $licenseDestination

    Write-Sha256Manifest -Directory $stageDir -Output (Join-Path $stageDir 'SHA256SUMS.txt')
    Compress-Archive -LiteralPath $stageDir -DestinationPath $archive -CompressionLevel Optimal
    Write-Host "Binary package: $archive"

    if (-not $SkipSourceBundle) {
        $sourceArguments = @{
            Version = $Version
            BuildDir = $buildDir
        }
        if ($DependencySourceRoot) {
            $sourceArguments.DependencySourceRoot = $DependencySourceRoot
        }
        & (Join-Path $PSScriptRoot 'package-source.ps1') @sourceArguments
    }

    $archives = @($archive)
    $sourceArchive = Join-Path $distDir "mosh-win-$Version-source.zip"
    if (Test-Path -LiteralPath $sourceArchive -PathType Leaf) {
        $archives += $sourceArchive
    }
    $archives | Sort-Object | ForEach-Object {
        "$(Get-Sha256 $_)  $(Split-Path -Leaf $_)"
    } | Set-Content -LiteralPath (Join-Path $distDir 'SHA256SUMS.txt') -Encoding ascii
} finally {
    Pop-Location
}

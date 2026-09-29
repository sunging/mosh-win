<#
.SYNOPSIS
Configures, builds and (unless -SkipTests) tests mosh-win with CMake presets.

.PARAMETER Offline
Do not download dependencies; the build directory must already contain
them, or -DependencySourceRoot must point at unpacked sources.

.PARAMETER DependencySourceRoot
Directory containing mosh_upstream/, protobuf/ and zlib/ source trees (for
example third_party/source from the corresponding-source package).

.PARAMETER MingwRoot
MinGW-w64 UCRT toolchain root. Defaults to $env:MINGW64_ROOT, then to the
toolchain whose gcc.exe is on PATH.
#>

# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Clean,
    [switch]$Offline,
    [switch]$SkipTests,
    [string]$BuildDirectory,
    [string]$DependencySourceRoot,
    [string]$MingwRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MoshWin.psm1') -Force

$root = Get-MoshWinRoot
$preset = Get-MoshWinPreset $Configuration
$buildDir = if ($BuildDirectory) {
    Resolve-MoshWinPath $BuildDirectory
} else {
    Join-Path $root "out/build/$preset"
}

$MingwRoot = Get-MingwRoot $MingwRoot
$mingwBin = Join-Path $MingwRoot 'bin'
foreach ($tool in @('gcc.exe', 'g++.exe', 'mingw32-make.exe')) {
    $toolPath = Join-Path $mingwBin $tool
    if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
        throw "Required MinGW64 tool was not found: $toolPath"
    }
}
$env:PATH = "$mingwBin;$env:PATH"

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path $root 'out/build'))
    if (-not $buildDir.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside $allowedRoot"
    }
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

$configureArgs = @('--preset', $preset, '-B', $buildDir,
    "-DMINGW64_ROOT=$($MingwRoot.Replace('\', '/'))")
if ($Offline) {
    $configureArgs += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'
}
if ($DependencySourceRoot) {
    $sourceRoot = Resolve-MoshWinPath $DependencySourceRoot
    foreach ($dependency in Get-MoshWinDependencyNames) {
        $source = Join-Path $sourceRoot $dependency
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "Bundled dependency source is missing: $source"
        }
        $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_$($dependency.ToUpperInvariant())=$source"
    }
}

Push-Location $root
try {
    Invoke-Native 'cmake' $configureArgs
    Invoke-Native 'cmake' @('--build', $buildDir, '--parallel', '4')

    if (-not $SkipTests) {
        Invoke-Native 'ctest' @('--test-dir', $buildDir, '--output-on-failure')
    }
} finally {
    Pop-Location
}

Write-Host "Build completed: $buildDir/bin"

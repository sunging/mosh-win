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
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset = if ($Configuration -eq 'Release') { 'mingw64-release' } else { 'mingw64-dev' }
$buildDir = if ($BuildDirectory) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    Join-Path $root "out/build/$preset"
}

function Invoke-Native([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File exited with code $LASTEXITCODE"
    }
}

# Resolve the MinGW-w64 toolchain: -MingwRoot, then $env:MINGW64_ROOT, then
# the directory holding gcc.exe on PATH.  No installation path is assumed.
if (-not $MingwRoot) { $MingwRoot = $env:MINGW64_ROOT }
if (-not $MingwRoot) {
    $gcc = Get-Command gcc.exe -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($gcc) { $MingwRoot = Split-Path -Parent (Split-Path -Parent $gcc.Source) }
}
if (-not $MingwRoot) {
    throw 'MinGW-w64 toolchain not found; pass -MingwRoot, set MINGW64_ROOT, or put its bin directory on PATH.'
}
$MingwRoot = [IO.Path]::GetFullPath($MingwRoot)
$mingwBin = Join-Path $MingwRoot 'bin'
foreach ($tool in @('gcc.exe', 'g++.exe', 'mingw32-make.exe')) {
    $toolPath = Join-Path $mingwBin $tool
    if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
        throw "Required MinGW64 tool was not found: $toolPath"
    }
}
$env:PATH = "$mingwBin;$env:PATH"

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    $fullBuildDir = [IO.Path]::GetFullPath($buildDir)
    $allowedRoot = [IO.Path]::GetFullPath((Join-Path $root 'out/build'))
    if (-not $fullBuildDir.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a path outside $allowedRoot"
    }
    Remove-Item -LiteralPath $fullBuildDir -Recurse -Force
}

$configureArgs = @('--preset', $preset, '-B', $buildDir,
    "-DMINGW64_ROOT=$($MingwRoot.Replace('\', '/'))")
if ($Offline) {
    $configureArgs += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON'
}
if ($DependencySourceRoot) {
    $sourceRoot = [IO.Path]::GetFullPath($DependencySourceRoot)
    foreach ($dependency in @('mosh_upstream', 'protobuf', 'zlib')) {
        $source = Join-Path $sourceRoot $dependency
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "Bundled dependency source is missing: $source"
        }
        $variable = "-DFETCHCONTENT_SOURCE_DIR_$($dependency.ToUpperInvariant())=$source"
        $configureArgs += $variable
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

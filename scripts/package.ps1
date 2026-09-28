[CmdletBinding()]
param(
    [string]$Version = '1.4.0-win1',
    [switch]$SkipBuild,
    [switch]$SkipSourceBundle,
    [string]$DependencySourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDir = Join-Path $root 'out/build/mingw64-release'
$binDir = Join-Path $buildDir 'bin'
$distDir = Join-Path $root 'dist'
$stageDir = Join-Path $distDir "mosh-win-$Version-x64"
$archive = "$stageDir.zip"

Push-Location $root
try {
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
        if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
    }

    $binaries = @((Join-Path $binDir 'mosh.exe'), (Join-Path $binDir 'mosh-client.exe'))
    & (Join-Path $PSScriptRoot 'audit-pe-imports.ps1') -Binary $binaries
    if ($LASTEXITCODE -ne 0) { throw 'PE import audit failed.' }

    New-Item -ItemType Directory -Path $distDir -Force | Out-Null
    if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    New-Item -ItemType Directory -Path $stageDir | Out-Null
    Copy-Item -LiteralPath $binaries -Destination $stageDir
    Copy-Item -LiteralPath @(
        (Join-Path $root 'README.md'),
        (Join-Path $root 'LICENSE'),
        (Join-Path $root 'THIRD_PARTY_NOTICES.md')) -Destination $stageDir
    $licenseSource = Join-Path $root 'third_party/licenses'
    $licenseDestination = Join-Path $stageDir 'licenses'
    if (-not (Test-Path -LiteralPath $licenseSource -PathType Container)) {
        throw "Bundled runtime licenses are missing: $licenseSource"
    }
    New-Item -ItemType Directory -Path $licenseDestination | Out-Null
    $licenseFiles = @(Get-ChildItem -LiteralPath $licenseSource -File)
    Copy-Item -LiteralPath $licenseFiles.FullName -Destination $licenseDestination

    $stagePrefix = $stageDir.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    Get-ChildItem -LiteralPath $stageDir -File -Recurse | Sort-Object FullName | ForEach-Object {
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
        $relative = $_.FullName.Substring($stagePrefix.Length).Replace('\', '/')
        "$($hash.Hash.ToLowerInvariant())  $relative"
    } | Set-Content -LiteralPath (Join-Path $stageDir 'SHA256SUMS.txt') -Encoding ascii

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
        if ($LASTEXITCODE -ne 0) { throw 'Corresponding-source package failed.' }
    }

    $archives = @($archive)
    $sourceArchive = Join-Path $distDir "mosh-win-$Version-source.zip"
    if (Test-Path -LiteralPath $sourceArchive -PathType Leaf) {
        $archives += $sourceArchive
    }
    $archives | Sort-Object | ForEach-Object {
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_
        "$($hash.Hash.ToLowerInvariant())  $(Split-Path -Leaf $_)"
    } | Set-Content -LiteralPath (Join-Path $distDir 'SHA256SUMS.txt') -Encoding ascii
} finally {
    Pop-Location
}

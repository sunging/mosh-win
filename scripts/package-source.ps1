[CmdletBinding()]
param(
    [string]$Version = '1.4.0-win1',
    [string]$BuildDir,
    [string]$DependencySourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $BuildDir) { $BuildDir = Join-Path $root 'out/build/mingw64-release' }
if ($DependencySourceRoot) {
    $DependencySourceRoot = [IO.Path]::GetFullPath($DependencySourceRoot)
}
$distDir = Join-Path $root 'dist'
$stageDir = Join-Path $distDir "mosh-win-$Version-source"
$archive = "$stageDir.zip"

if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

Push-Location $root
try {
    & robocopy $root (Join-Path $stageDir 'mosh-win') /E /XD .git out dist build .agents .cache .codex .vscode .idea /XF '*.user' CMakeUserPresets.json | Out-Host
    $copyResult = $LASTEXITCODE
    if ($copyResult -ge 8) { throw "robocopy failed with code $copyResult" }

    $thirdPartyDir = Join-Path $stageDir 'third_party/source'
    New-Item -ItemType Directory -Path $thirdPartyDir -Force | Out-Null
    foreach ($dependency in @('mosh_upstream', 'protobuf', 'zlib')) {
        $source = if ($DependencySourceRoot) {
            Join-Path $DependencySourceRoot $dependency
        } else {
            Join-Path $BuildDir "_deps/$dependency-src"
        }
        if (-not (Test-Path -LiteralPath $source -PathType Container)) {
            throw "Corresponding dependency source is missing: $source. Configure the release preset first or pass -DependencySourceRoot."
        }
        & robocopy $source (Join-Path $thirdPartyDir $dependency) /E | Out-Host
        $copyResult = $LASTEXITCODE
        if ($copyResult -ge 8) { throw "robocopy failed with code $copyResult" }
    }

    $stagePrefix = $stageDir.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    Get-ChildItem -LiteralPath $stageDir -File -Recurse | Sort-Object FullName | ForEach-Object {
        $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName
        $relative = $_.FullName.Substring($stagePrefix.Length).Replace('\', '/')
        "$($hash.Hash.ToLowerInvariant())  $relative"
    } | Set-Content -LiteralPath (Join-Path $stageDir 'SHA256SUMS.txt') -Encoding ascii

    Compress-Archive -LiteralPath $stageDir -DestinationPath $archive -CompressionLevel Optimal
    Write-Host "Corresponding-source package: $archive"
} finally {
    Pop-Location
}

exit 0

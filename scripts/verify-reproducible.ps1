[CmdletBinding()]
param(
    [string]$DependencySourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$primary = Join-Path $root 'out/build/mingw64-release'
$secondary = Join-Path $root 'out/build/mingw64-repro'

$common = @{
    Configuration = 'Release'
    Clean = $true
}
if ($DependencySourceRoot) {
    $common.DependencySourceRoot = [IO.Path]::GetFullPath($DependencySourceRoot)
    $common.Offline = $true
}

& (Join-Path $PSScriptRoot 'build.ps1') @common -BuildDirectory $primary
& (Join-Path $PSScriptRoot 'build.ps1') @common -BuildDirectory $secondary

$mismatch = $false
$results = foreach ($name in @('mosh.exe', 'mosh-client.exe')) {
    $primaryPath = Join-Path $primary "bin/$name"
    $secondaryPath = Join-Path $secondary "bin/$name"
    $primaryHash = (Get-FileHash -LiteralPath $primaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $secondaryHash = (Get-FileHash -LiteralPath $secondaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
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

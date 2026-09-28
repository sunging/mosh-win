# Shared helpers for the mosh-win build, test and packaging scripts.
#
# Every function throws on failure, so callers only need
# $ErrorActionPreference = 'Stop' and never inspect $LASTEXITCODE themselves.

Set-StrictMode -Version Latest

# Dependency trees that FetchContent populates and the source bundle ships.
$script:DependencyNames = @('mosh_upstream', 'protobuf', 'zlib')

function Get-MoshWinRoot {
    <# Returns the absolute path of the repository root. #>
    [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
}

function Resolve-MoshWinPath {
    <#
    Resolves a possibly relative path against the PowerShell location.
    [IO.Path]::GetFullPath alone would use the process working directory,
    which Set-Location does not change.
    #>
    param([Parameter(Mandatory)][string]$Path)
    $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Get-MoshWinVersion {
    <# Returns the package version (for example 1.4.0-win1) from CMakeLists.txt. #>
    $cmakeLists = Get-Content -Raw -LiteralPath (Join-Path (Get-MoshWinRoot) 'CMakeLists.txt')
    $upstream = [regex]::Match($cmakeLists, '(?s)project\(mosh-win\s+VERSION\s+([0-9.]+)')
    $revision = [regex]::Match($cmakeLists, 'set\(MOSH_WIN_PACKAGE_REVISION\s+"([^"]+)"\)')
    if (-not $upstream.Success -or -not $revision.Success) {
        throw 'Could not read the project version from CMakeLists.txt'
    }
    "$($upstream.Groups[1].Value)-$($revision.Groups[1].Value)"
}

function Get-MoshWinPreset {
    <# Maps a build configuration to its CMake preset name. #>
    param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
    if ($Configuration -eq 'Release') { 'mingw64-release' } else { 'mingw64-dev' }
}

function Get-MoshWinDependencyNames {
    <# Names of the FetchContent dependencies, as used in _deps/<name>-src. #>
    $script:DependencyNames
}

function Get-MingwRoot {
    <#
    Locates the MinGW-w64 UCRT toolchain: the explicit -MingwRoot argument,
    then $env:MINGW64_ROOT, then the parent of the directory containing
    gcc.exe on PATH.  No installation directory is assumed.
    #>
    param([string]$MingwRoot)
    if (-not $MingwRoot) { $MingwRoot = $env:MINGW64_ROOT }
    if (-not $MingwRoot) {
        $gcc = Get-Command gcc.exe -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($gcc) { $MingwRoot = Split-Path -Parent (Split-Path -Parent $gcc.Source) }
    }
    if (-not $MingwRoot) {
        throw 'MinGW-w64 toolchain not found; pass -MingwRoot, set MINGW64_ROOT, or put its bin directory on PATH.'
    }
    $MingwRoot = Resolve-MoshWinPath $MingwRoot
    if (-not (Test-Path -LiteralPath (Join-Path $MingwRoot 'bin/gcc.exe') -PathType Leaf)) {
        throw "MinGW-w64 root does not contain bin/gcc.exe: $MingwRoot"
    }
    $MingwRoot
}

function Invoke-Native {
    <# Runs a native command and throws if it exits with a non-zero status. #>
    param(
        [Parameter(Mandatory)][string]$File,
        [string[]]$Arguments = @()
    )
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File exited with code $LASTEXITCODE"
    }
}

function Invoke-Robocopy {
    <# Mirrors a directory tree; robocopy uses exit codes below 8 for success. #>
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Destination,
        [string[]]$Options = @()
    )
    & robocopy $Source $Destination /E @Options | Out-Host
    if ($LASTEXITCODE -ge 8) {
        throw "robocopy failed with code $LASTEXITCODE"
    }
    $global:LASTEXITCODE = 0
}

function Reset-Directory {
    <# Deletes a directory tree if present and recreates it empty. #>
    param([Parameter(Mandatory)][string]$Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Get-Sha256 {
    <# Returns the lower-case SHA-256 digest of a file. #>
    param([Parameter(Mandatory)][string]$Path)
    (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Write-Sha256Manifest {
    <#
    Writes "<sha256>  <relative/path>" lines (sha256sum format) for every
    file below -Directory, sorted by path, to -Output.
    #>
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][string]$Output
    )
    $prefix = $Directory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($prefix.Length).Replace('\', '/')
        "$(Get-Sha256 $_.FullName)  $relative"
    } | Set-Content -LiteralPath $Output -Encoding ascii
}

Export-ModuleMember -Function Get-MoshWinRoot, Resolve-MoshWinPath, Get-MoshWinVersion,
    Get-MoshWinPreset, Get-MoshWinDependencyNames, Get-MingwRoot, Invoke-Native,
    Invoke-Robocopy, Reset-Directory, Get-Sha256, Write-Sha256Manifest

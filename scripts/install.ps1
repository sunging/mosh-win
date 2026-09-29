<#
.SYNOPSIS
Builds the release and installs it, or removes an installation made by this
script.

.DESCRIPTION
Runs the CMake install target into -Prefix: mosh.exe and mosh-client.exe go
to <Prefix>\bin, the documentation and bundled licenses to
<Prefix>\share\doc\mosh-win. The list of installed files is kept in
<Prefix>\share\doc\mosh-win\install_manifest.txt so that -Uninstall removes
exactly those files.

.PARAMETER Prefix
Installation root. Defaults to %LOCALAPPDATA%\Programs\mosh-win, which needs
no administrator rights.

.PARAMETER SkipBuild
Install the existing release build instead of building and testing first.

.PARAMETER AddToPath
Add <Prefix>\bin to the current user's PATH if it is not there yet. New
terminals see the change.

.PARAMETER Uninstall
Remove the files listed in the installation's manifest, the directories
left empty and the <Prefix>\bin entry in the user's PATH.

.PARAMETER BuildDirectory
Build directory; defaults to out\build\mingw64-release. Also passed to
build.ps1, as are -Offline, -DependencySourceRoot and -MingwRoot.

.EXAMPLE
.\scripts\install.ps1 -AddToPath

.EXAMPLE
.\scripts\install.ps1 -Uninstall
#>

# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [string]$Prefix,
    [Parameter(ParameterSetName = 'Install')][switch]$SkipBuild,
    [Parameter(ParameterSetName = 'Install')][switch]$AddToPath,
    [Parameter(ParameterSetName = 'Install')][string]$BuildDirectory,
    [Parameter(ParameterSetName = 'Install')][switch]$Offline,
    [Parameter(ParameterSetName = 'Install')][string]$DependencySourceRoot,
    [Parameter(ParameterSetName = 'Install')][string]$MingwRoot,
    [Parameter(Mandatory, ParameterSetName = 'Uninstall')][switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'MoshWin.psm1') -Force

if ($Prefix) {
    $Prefix = Resolve-MoshWinPath $Prefix
} elseif ($env:LOCALAPPDATA) {
    $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\mosh-win'
} else {
    throw 'LOCALAPPDATA is not set; pass -Prefix'
}
$Prefix = $Prefix.TrimEnd('\', '/')
$binDir = Join-Path $Prefix 'bin'
$docDir = Join-Path $Prefix 'share\doc\mosh-win'
$manifest = Join-Path $docDir 'install_manifest.txt'

function Test-SamePath([string]$Left, [string]$Right) {
    $normalize = {
        param($Path)
        [Environment]::ExpandEnvironmentVariables($Path).Trim().TrimEnd('\', '/').Replace('/', '\')
    }
    [string]::Equals((& $normalize $Left), (& $normalize $Right),
        [StringComparison]::OrdinalIgnoreCase)
}

# Adds or removes one entry of the user's PATH and leaves every other byte of
# the value alone, so that -AddToPath followed by -Uninstall restores it
# exactly.  HKCU\Environment is edited directly because
# [Environment]::SetEnvironmentVariable would store PATH as REG_SZ with every
# %VARIABLE% reference expanded.
function Update-UserPath([string]$Entry, [switch]$Remove) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    try {
        $current = [string]$key.GetValue('Path', '',
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $entries = $current -split ';'
        $present = @($entries | Where-Object { $_.Trim() -and (Test-SamePath $_ $Entry) })
        if ($Remove) {
            if (-not $present) { return $false }
            $updated = ($entries | Where-Object {
                -not ($_.Trim() -and (Test-SamePath $_ $Entry))
            }) -join ';'
        } else {
            if ($present) { return $false }
            $updated = if (-not $current) {
                $Entry
            } elseif ($current.EndsWith(';')) {
                "$current$Entry;"
            } else {
                "$current;$Entry"
            }
        }
        $key.SetValue('Path', $updated,
            [Microsoft.Win32.RegistryValueKind]::ExpandString)
    } finally {
        $key.Dispose()
    }

    # Tell Explorer and other running programs that the environment changed.
    if (-not ('MoshWin.NativeMethods' -as [type])) {
        Add-Type -Namespace MoshWin -Name NativeMethods -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg,
    UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout,
    out UIntPtr lpdwResult);
'@
    }
    $result = [UIntPtr]::Zero
    [void][MoshWin.NativeMethods]::SendMessageTimeout([IntPtr]0xffff, 0x1A,
        [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result)
    return $true
}

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "No installation manifest at $manifest"
    }
    $files = @(Get-Content -LiteralPath $manifest | Where-Object { $_ })
    $prefixWithSeparator = $Prefix + '\'
    foreach ($file in $files) {
        $path = [IO.Path]::GetFullPath($file)
        if (-not $path.StartsWith($prefixWithSeparator, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Manifest entry is outside ${Prefix}: $file"
        }
    }
    foreach ($file in $files + $manifest) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            Remove-Item -LiteralPath $file -Force
        }
    }

    # Prune directories the installation created, deepest first, but only
    # when they are empty.
    $directories = $files | ForEach-Object {
        $directory = Split-Path -Parent ([IO.Path]::GetFullPath($_))
        while ($directory.Length -gt $Prefix.Length) {
            $directory
            $directory = Split-Path -Parent $directory
        }
    } | Sort-Object -Unique | Sort-Object Length -Descending
    foreach ($directory in @($directories) + $Prefix) {
        if ((Test-Path -LiteralPath $directory -PathType Container) -and
            -not (Get-ChildItem -LiteralPath $directory -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $directory -Force
        }
    }

    $removed = Update-UserPath $binDir -Remove
    if ($removed) { Write-Host "Removed $binDir from the user PATH" }
    Write-Host "Uninstalled mosh-win from $Prefix"
    return
}

$root = Get-MoshWinRoot
$buildDir = if ($BuildDirectory) {
    Resolve-MoshWinPath $BuildDirectory
} else {
    Join-Path $root "out/build/$(Get-MoshWinPreset Release)"
}

if (-not $SkipBuild) {
    $buildArguments = @{ Configuration = 'Release'; BuildDirectory = $buildDir }
    if ($Offline) { $buildArguments.Offline = $true }
    if ($DependencySourceRoot) { $buildArguments.DependencySourceRoot = $DependencySourceRoot }
    if ($MingwRoot) { $buildArguments.MingwRoot = $MingwRoot }
    & (Join-Path $PSScriptRoot 'build.ps1') @buildArguments
}
if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'cmake_install.cmake') -PathType Leaf)) {
    throw "No configured build in $buildDir; run without -SkipBuild"
}

Invoke-Native 'cmake' @('--install', $buildDir, '--prefix', $Prefix)

# cmake --install writes the manifest into the build directory, where the next
# install (including the install.layout test) overwrites it.  Keep a copy with
# the installation for -Uninstall.
$installed = @(Get-Content -LiteralPath (Join-Path $buildDir 'install_manifest.txt') |
    Where-Object { $_ } |
    ForEach-Object { [IO.Path]::GetFullPath($_) })
Set-Content -LiteralPath $manifest -Value $installed -Encoding utf8

if ($AddToPath) {
    $added = Update-UserPath $binDir
    if ($added) {
        Write-Host "Added $binDir to the user PATH; open a new terminal to use it"
    } else {
        Write-Host "$binDir is already on the user PATH"
    }
}

Write-Host "Installed mosh-win to $Prefix"

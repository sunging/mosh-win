[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]]$Binary,
    [string]$Objdump,
    [switch]$AllowUnknownSystemDll
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $Objdump) {
    if ($env:MINGW64_ROOT) {
        $Objdump = Join-Path $env:MINGW64_ROOT 'bin/objdump.exe'
    } else {
        $found = Get-Command objdump.exe -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if (-not $found) {
            throw 'objdump.exe not found; pass -Objdump, set MINGW64_ROOT, or put the MinGW-w64 bin directory on PATH.'
        }
        $Objdump = $found.Source
    }
}
if (-not (Test-Path -LiteralPath $Objdump -PathType Leaf)) {
    throw "objdump was not found: $Objdump"
}

# Runtime DLLs that must never be imported: POSIX emulation layers, the
# legacy MSVCRT, MinGW C++/GCC runtimes and third-party libraries that are
# supposed to be linked statically.
$forbidden = @(
    '^cygwin1\.dll$', '^msys-2\.0\.dll$',
    '^msvcrt\.dll$',
    '^libstdc\+\+-6\.dll$', '^libgcc_s_.*\.dll$', '^libwinpthread-1\.dll$',
    '^libprotobuf.*\.dll$', '^zlib1\.dll$', '^libzstd\.dll$',
    '^libcrypto.*\.dll$', '^libssl.*\.dll$', '^.*ncurses.*\.dll$')

# Reviewed operating-system DLLs.
$allowed = @(
    '^kernel32\.dll$', '^ntdll\.dll$', '^user32\.dll$', '^gdi32\.dll$',
    '^ws2_32\.dll$', '^bcrypt\.dll$', '^advapi32\.dll$', '^shell32\.dll$',
    '^ole32\.dll$', '^oleaut32\.dll$', '^crypt32\.dll$', '^iphlpapi\.dll$',
    '^secur32\.dll$', '^userenv\.dll$', '^shlwapi\.dll$', '^version\.dll$',
    '^ucrtbase\.dll$',
    '^api-ms-win-.*\.dll$', '^ext-ms-win-.*\.dll$')

# Collect every problem before failing so one run reports all of them.
# (Write-Error would terminate on the first one under ErrorActionPreference
# 'Stop'.)
$problems = [Collections.Generic.List[string]]::new()

foreach ($path in $Binary) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $problems.Add("PE binary was not found: $path")
        continue
    }

    $dump = & $Objdump -p $path
    if ($LASTEXITCODE -ne 0) {
        $problems.Add("objdump failed for $path")
        continue
    }

    $imports = @($dump | ForEach-Object {
        if ($_ -match '^\s*DLL Name:\s*(\S+)\s*$') { $Matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
    Write-Host "$(Split-Path -Leaf $path): $($imports -join ', ')"

    foreach ($dll in $imports) {
        if ($forbidden | Where-Object { $dll -match $_ }) {
            $problems.Add("Forbidden runtime dependency in ${path}: $dll")
            continue
        }
        if (-not $AllowUnknownSystemDll -and
            -not ($allowed | Where-Object { $dll -match $_ })) {
            $problems.Add("Non-system or unreviewed runtime dependency in ${path}: $dll")
        }
    }
}

if ($problems.Count -ne 0) {
    foreach ($problem in $problems) {
        [Console]::Error.WriteLine("error: $problem")
    }
    [Console]::Error.WriteLine("PE import audit failed with $($problems.Count) problem(s).")
    exit 1
}
Write-Host 'PE import audit passed.'

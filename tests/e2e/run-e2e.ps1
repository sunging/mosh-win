<#
.SYNOPSIS
End-to-end test: Windows OpenSSH -> Linux mosh-server -> native mosh-client.

.DESCRIPTION
Runs mosh.exe against a Linux host and requires a clean session shutdown.

Any Linux host works: a physical machine, VM, cloud instance, container or
WSL distribution. It must be reachable with the Windows OpenSSH client using
non-interactive authentication (key or agent), have mosh-server installed and
a UTF-8 locale, and accept Mosh's UDP traffic from this machine:

    run-e2e.ps1 -Target user@linux-host [-SshPort 22] [-IdentityFile key]

With -Wsl the script instead starts a temporary, key-only sshd inside a local
WSL distribution (no persistent service, no stored credentials) and tests
against it:

    run-e2e.ps1 -Wsl [-Distro Ubuntu] [-InstallOpenSshServer]

mosh-client.exe needs a real console. Run the script from a terminal window,
or add -NewWindow to run it in a new console window and wait for the result.

.PARAMETER Target
[user@]host for ssh; an ssh_config alias works. The host's key must already
be known (for example from a previous interactive ssh login).

.PARAMETER SshOption
Extra OpenSSH arguments, for example -SshOption '-o', 'ProxyJump=bastion'.
They are used for the pre-flight check and passed to mosh.exe.

.PARAMETER ServerAddress
UDP address of the server when it differs from the resolved SSH HostName.

.PARAMETER SshPort
SSH port. 0 uses ssh_config or the default; with -Wsl it selects the
fixture's port (default 22222).

.PARAMETER UdpPort
Server UDP port (mosh -p). 0 lets mosh-server choose; with -Wsl the default
is 61234.

.PARAMETER SshOnly
Only run the SSH pre-flight check, not Mosh itself.
#>

# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding(DefaultParameterSetName = 'Host')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Host')]
    [string]$Target,
    [Parameter(ParameterSetName = 'Host')]
    [string]$IdentityFile,
    [Parameter(ParameterSetName = 'Host')]
    [string[]]$SshOption = @(),
    [Parameter(ParameterSetName = 'Host')]
    [string]$ServerAddress,

    [Parameter(Mandatory, ParameterSetName = 'Wsl')]
    [switch]$Wsl,
    [Parameter(ParameterSetName = 'Wsl')]
    [string]$Distro = 'Ubuntu',
    [Parameter(ParameterSetName = 'Wsl')]
    [switch]$InstallOpenSshServer,
    [Parameter(ParameterSetName = 'Wsl')]
    [switch]$KeepFixture,

    [ValidateRange(0, 65535)]
    [int]$SshPort = 0,
    [ValidateRange(0, 65535)]
    [int]$UdpPort = 0,
    [string]$BuildDirectory = 'out/build/mingw64-release/bin',
    [switch]$SshOnly,
    [switch]$NewWindow
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------
# -NewWindow: re-run this script with the same parameters in a new console
# window, wait for it, and report its transcript and result here.
# ---------------------------------------------------------------------------
if ($NewWindow) {
    $forwarded = @{}
    foreach ($name in $PSBoundParameters.Keys) {
        if ($name -eq 'NewWindow') { continue }
        $value = $PSBoundParameters[$name]
        if ($value -is [switch]) { $value = $value.IsPresent }
        $forwarded[$name] = $value
    }
    $log = Join-Path ([IO.Path]::GetTempPath()) ("mosh-win-e2e-" + [Guid]::NewGuid().ToString('N') + '.log')
    $payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(
        [Management.Automation.PSSerializer]::Serialize($forwarded)))
    $quotedScript = $PSCommandPath.Replace("'", "''")
    $quotedLog = $log.Replace("'", "''")
    $child = @"
`$ErrorActionPreference = 'Stop'
`$parameters = [Management.Automation.PSSerializer]::Deserialize(
    [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$payload')))
Start-Transcript -LiteralPath '$quotedLog' | Out-Null
`$code = 0
try { & '$quotedScript' @parameters }
catch { Write-Host "E2E FAILED: `$(`$_.Exception.Message)"; `$code = 1 }
Stop-Transcript | Out-Null
exit `$code
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))
    $shell = (Get-Process -Id $PID).Path
    $process = Start-Process -FilePath $shell -Wait -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded)
    if (Test-Path -LiteralPath $log) {
        # Print only the transcript body: the header and footer blocks are
        # delimited by lines of asterisks and hold host/user details.
        $lines = @(Get-Content -LiteralPath $log)
        $rules = @(for ($i = 0; $i -lt $lines.Count; ++$i) {
            if ($lines[$i] -match '^\*{10,}$') { $i }
        })
        if ($rules.Count -ge 3 -and $rules[2] - $rules[1] -gt 1) {
            $lines[($rules[1] + 1)..($rules[2] - 1)] | Write-Host
        } elseif ($rules.Count -lt 3) {
            $lines | Write-Host
        }
        Remove-Item -LiteralPath $log -Force
    }
    if ($process.ExitCode -ne 0) {
        throw "end-to-end test failed in the new console window (exit code $($process.ExitCode))"
    }
    return
}

if (-not $SshOnly -and ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected)) {
    throw 'mosh-client.exe needs a console, but standard input or output is redirected. Run this script from a terminal window, or add -NewWindow.'
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$MoshExe = Join-Path (Join-Path $ProjectRoot $BuildDirectory) 'mosh.exe'
if (-not $SshOnly -and -not (Test-Path -LiteralPath $MoshExe -PathType Leaf)) {
    throw "mosh.exe was not found at $MoshExe; build the Release preset first"
}

$System32 = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
$SshExe = Join-Path $System32 'OpenSSH\ssh.exe'
$SshKeygenExe = Join-Path $System32 'OpenSSH\ssh-keygen.exe'
foreach ($RequiredFile in @($SshExe, $SshKeygenExe)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "required Windows OpenSSH program was not found: $RequiredFile"
    }
}

# OpenSSH arguments shared by the pre-flight check and mosh.exe.  BatchMode
# makes a missing key or unknown host key fail instead of prompting.
$SshArguments = [Collections.Generic.List[string]]::new()
$SshArguments.AddRange([string[]]@('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10'))

# ---------------------------------------------------------------------------
# WSL fixture: a temporary sshd with a throw-away host key and client key.
# ---------------------------------------------------------------------------
function Invoke-Wsl {
    param([Parameter(Mandatory)][string[]]$Arguments)
    & wsl.exe -d $Distro -- @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

function Invoke-WslRoot {
    param([Parameter(Mandatory)][string[]]$Arguments)
    & wsl.exe -d $Distro -u root -- @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "WSL root command failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

$FixtureStarted = $false
$FixtureWslPath = $null
$StateDirectory = $null

try {
    if ($PSCmdlet.ParameterSetName -eq 'Wsl') {
        if ($SshPort -eq 0) { $SshPort = 22222 }
        if ($SshPort -lt 1024) { throw 'the WSL fixture needs an SSH port of 1024 or higher' }
        if ($UdpPort -eq 0) { $UdpPort = 61234 }

        & wsl.exe -d $Distro -- test -x /usr/sbin/sshd
        if ($LASTEXITCODE -ne 0) {
            if (-not $InstallOpenSshServer) {
                throw "openssh-server is not installed in $Distro; rerun with -InstallOpenSshServer"
            }
            Invoke-WslRoot -Arguments @('apt-get', 'update')
            Invoke-WslRoot -Arguments @('env', 'DEBIAN_FRONTEND=noninteractive', 'apt-get', 'install', '-y', 'openssh-server')
            # Ubuntu on WSL may not use systemd as PID 1.  Disable any generated
            # systemd unit when available and also stop the SysV service, but do
            # not fail merely because there is no service manager.
            Invoke-WslRoot -Arguments @(
                'sh', '-c',
                'systemctl disable ssh >/dev/null 2>&1 || true; service ssh stop >/dev/null 2>&1 || true')
        }

        $FixtureWindowsPath = (Resolve-Path (Join-Path $PSScriptRoot 'wsl-sshd-fixture.sh')).Path
        if ($FixtureWindowsPath -notmatch '^([A-Za-z]):\\(.*)$') {
            throw "the WSL fixture must be on a drive-letter path: $FixtureWindowsPath"
        }
        $FixtureWslPath = "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2].Replace('\', '/'))"

        $StateDirectory = Join-Path ([IO.Path]::GetTempPath()) ('mosh-win-e2e-' + [Guid]::NewGuid().ToString('N'))
        $PrivateKey = Join-Path $StateDirectory 'id_ed25519'
        New-Item -ItemType Directory -Path $StateDirectory | Out-Null

        # Windows PowerShell 5.1 drops empty native-command arguments.  Preserve
        # the required empty passphrase by passing one quoted command-line string.
        $EscapedPrivateKey = $PrivateKey.Replace('"', '\"')
        $KeygenArguments = "-q -t ed25519 -N `"`" -f `"$EscapedPrivateKey`""
        $KeygenProcess = Start-Process -FilePath $SshKeygenExe `
            -ArgumentList $KeygenArguments -NoNewWindow -Wait -PassThru
        if ($KeygenProcess.ExitCode -ne 0) {
            throw "ssh-keygen failed with exit code $($KeygenProcess.ExitCode)"
        }

        $PublicKeyBytes = [Text.Encoding]::UTF8.GetBytes((Get-Content -LiteralPath "$PrivateKey.pub" -Raw).Trim())
        $PublicKeyBase64 = [Convert]::ToBase64String($PublicKeyBytes)
        $FixtureLoginUser = (& wsl.exe -d $Distro -- id -un).Trim()
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($FixtureLoginUser)) {
            throw "could not determine the default user for $Distro"
        }
        $FixtureOutput = & wsl.exe -d $Distro -u root -- bash $FixtureWslPath start $SshPort $PublicKeyBase64 $FixtureLoginUser
        if ($LASTEXITCODE -ne 0) {
            throw 'the WSL sshd fixture failed to start'
        }
        $FixtureStarted = $true

        $FixtureValues = @{}
        foreach ($Line in $FixtureOutput) {
            if ($Line -match '^(MOSH_TEST_[A-Z_]+)=(.*)$') {
                $FixtureValues[$Matches[1]] = $Matches[2].Trim()
            }
        }
        foreach ($Name in @('MOSH_TEST_HOST', 'MOSH_TEST_USER')) {
            if (-not $FixtureValues.ContainsKey($Name)) {
                throw "the WSL fixture did not report $Name"
            }
        }

        $ServerAddress = $FixtureValues['MOSH_TEST_HOST']
        $Target = "$($FixtureValues['MOSH_TEST_USER'])@$ServerAddress"
        # The fixture's host key is created per run, so it cannot be known.
        $SshArguments.AddRange([string[]]@(
            '-p', "$SshPort",
            '-i', $PrivateKey,
            '-o', 'IdentitiesOnly=yes',
            '-o', 'StrictHostKeyChecking=no',
            '-o', 'UserKnownHostsFile=NUL'))
    } else {
        if ($SshPort -ne 0) { $SshArguments.AddRange([string[]]@('-p', "$SshPort")) }
        if ($IdentityFile) {
            $IdentityPath = (Resolve-Path -LiteralPath $IdentityFile).Path
            $SshArguments.AddRange([string[]]@('-i', $IdentityPath, '-o', 'IdentitiesOnly=yes'))
        }
        $SshArguments.AddRange([string[]]$SshOption)
    }

    # -----------------------------------------------------------------------
    # Pre-flight: the host must be Linux with mosh-server and a UTF-8 locale.
    # The probe avoids double quotes so it survives Windows argument passing,
    # and is interpreted by the remote login shell.
    # -----------------------------------------------------------------------
    $Probe = 'echo MOSH_E2E_OS=$(uname -s); echo MOSH_E2E_SERVER=$(command -v mosh-server); echo MOSH_E2E_CHARMAP=$(locale charmap 2>/dev/null)'
    $ProbeOutput = & $SshExe -n @($SshArguments.ToArray()) $Target -- $Probe
    $ProbeExit = $LASTEXITCODE
    $Facts = @{}
    foreach ($Line in @($ProbeOutput)) {
        if ($Line -match '^(MOSH_E2E_[A-Z]+)=(.*)$') {
            $Facts[$Matches[1]] = $Matches[2].Trim()
        }
    }
    if (-not $Facts.ContainsKey('MOSH_E2E_OS')) {
        throw "Windows OpenSSH could not run a command on $Target (exit code $ProbeExit). Check that key or agent authentication works without prompts and that the host key is known."
    }
    if ($ProbeExit -ne 0) {
        Write-Warning "Windows OpenSSH returned $ProbeExit after the remote command succeeded (Win32-OpenSSH issue #1899)"
    }
    if ($Facts['MOSH_E2E_OS'] -ne 'Linux') {
        throw "$Target is not a Linux host (uname -s: '$($Facts['MOSH_E2E_OS'])')"
    }
    if (-not $Facts['MOSH_E2E_SERVER']) {
        throw "mosh-server was not found on $Target; install the mosh package there"
    }
    if ($Facts['MOSH_E2E_CHARMAP'] -notmatch '^(?i)utf-?8$') {
        throw "the SSH session on $Target does not use a UTF-8 locale (locale charmap: '$($Facts['MOSH_E2E_CHARMAP'])'), which mosh-server requires. Configure a UTF-8 default locale on the host, for example LANG=C.UTF-8."
    }
    Write-Host "Pre-flight passed: $Target is Linux, mosh-server at $($Facts['MOSH_E2E_SERVER']), UTF-8 locale"

    if (-not $SshOnly) {
        $MoshArguments = [Collections.Generic.List[string]]::new()
        foreach ($Argument in $SshArguments) {
            $MoshArguments.Add("--ssh-option=$Argument")
        }
        if ($ServerAddress) { $MoshArguments.Add("--server-address=$ServerAddress") }
        if ($UdpPort -ne 0) { $MoshArguments.AddRange([string[]]@('-p', "$UdpPort")) }
        $MoshArguments.Add($Target)
        # Remote command: prints a marker and exits, which ends the session.
        $MoshArguments.AddRange([string[]]@('sh', '-c', "printf 'MOSH_NATIVE_E2E_OK\n'; sleep 1"))

        & $MoshExe @($MoshArguments.ToArray())
        if ($LASTEXITCODE -ne 0) {
            throw "native Mosh session to $Target failed with exit code $LASTEXITCODE (is its UDP port reachable from this machine?)"
        }
    }

    Write-Host "End-to-end test passed for $Target"
}
finally {
    if ($FixtureStarted) {
        if ($KeepFixture) {
            Write-Host "WSL fixture kept: ssh -p $SshPort -i `"$PrivateKey`" $Target"
            Write-Host "Stop it with: wsl.exe -d $Distro -u root -- bash $FixtureWslPath stop"
        } else {
            & wsl.exe -d $Distro -u root -- bash $FixtureWslPath stop | Out-Null
        }
    }
    if ($StateDirectory -and -not $KeepFixture) {
        Remove-Item -LiteralPath $StateDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

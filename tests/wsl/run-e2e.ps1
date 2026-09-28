[CmdletBinding()]
param(
    [string]$Distro = "Ubuntu",
    [string]$BuildDirectory = "out/build/mingw64-release/bin",
    [ValidateRange(1024, 65535)]
    [int]$SshPort = 22222,
    [ValidateRange(1, 65535)]
    [int]$UdpPort = 61234,
    [switch]$InstallOpenSshServer,
    [switch]$KeepFixture,
    [switch]$SshOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Invoke-Wsl {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    & wsl.exe -d $Distro -- @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

function Invoke-WslRoot {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    & wsl.exe -d $Distro -u root -- @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "WSL root command failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$MoshExe = Join-Path (Join-Path $ProjectRoot $BuildDirectory) "mosh.exe"
if (-not (Test-Path -LiteralPath $MoshExe -PathType Leaf)) {
    throw "mosh.exe was not found at $MoshExe; build the Release preset first"
}

$System32 = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
$SshExe = Join-Path $System32 "OpenSSH\ssh.exe"
$SshKeygenExe = Join-Path $System32 "OpenSSH\ssh-keygen.exe"
foreach ($RequiredFile in @($SshExe, $SshKeygenExe)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "required Windows OpenSSH program was not found: $RequiredFile"
    }
}

& wsl.exe -d $Distro -- test -x /usr/sbin/sshd
if ($LASTEXITCODE -ne 0) {
    if (-not $InstallOpenSshServer) {
        throw "openssh-server is not installed in $Distro; rerun with -InstallOpenSshServer"
    }
    Invoke-WslRoot -Arguments @("apt-get", "update")
    Invoke-WslRoot -Arguments @("env", "DEBIAN_FRONTEND=noninteractive", "apt-get", "install", "-y", "openssh-server")
    # Ubuntu on WSL may not use systemd as PID 1.  Disable any generated
    # systemd unit when available and also stop the SysV service, but do not
    # fail merely because there is no service manager in this distribution.
    Invoke-WslRoot -Arguments @(
        "sh", "-c",
        "systemctl disable ssh >/dev/null 2>&1 || true; service ssh stop >/dev/null 2>&1 || true"
    )
}

$FixtureWindowsPath = (Resolve-Path (Join-Path $PSScriptRoot "sshd-fixture.sh")).Path
if ($FixtureWindowsPath -notmatch '^([A-Za-z]):\\(.*)$') {
    throw "the WSL fixture must be on a drive-letter path: $FixtureWindowsPath"
}
$FixtureDrive = $Matches[1].ToLowerInvariant()
$FixtureRelativePath = $Matches[2].Replace('\', '/')
$FixtureWslPath = "/mnt/$FixtureDrive/$FixtureRelativePath"

$StateDirectory = Join-Path ([IO.Path]::GetTempPath()) ("mosh-win-e2e-" + [Guid]::NewGuid().ToString("N"))
$PrivateKey = Join-Path $StateDirectory "id_ed25519"
$PublicKey = "$PrivateKey.pub"
New-Item -ItemType Directory -Path $StateDirectory | Out-Null

$FixtureStarted = $false
try {
    # Windows PowerShell 5.1 drops empty native-command arguments.  Preserve
    # the required empty passphrase by passing one quoted command-line string.
    $EscapedPrivateKey = $PrivateKey.Replace('"', '\"')
    $KeygenArguments = "-q -t ed25519 -N `"`" -f `"$EscapedPrivateKey`""
    $KeygenProcess = Start-Process -FilePath $SshKeygenExe `
        -ArgumentList $KeygenArguments -NoNewWindow -Wait -PassThru
    if ($KeygenProcess.ExitCode -ne 0) {
        throw "ssh-keygen failed with exit code $($KeygenProcess.ExitCode)"
    }

    $PublicKeyBytes = [Text.Encoding]::UTF8.GetBytes((Get-Content -LiteralPath $PublicKey -Raw).Trim())
    $PublicKeyBase64 = [Convert]::ToBase64String($PublicKeyBytes)
    $FixtureLoginUser = (& wsl.exe -d $Distro -- id -un).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($FixtureLoginUser)) {
        throw "could not determine the default user for $Distro"
    }
    $FixtureOutput = & wsl.exe -d $Distro -u root -- bash $FixtureWslPath start $SshPort $PublicKeyBase64 $FixtureLoginUser
    if ($LASTEXITCODE -ne 0) {
        throw "the WSL sshd fixture failed to start"
    }
    $FixtureStarted = $true

    $FixtureValues = @{}
    foreach ($Line in $FixtureOutput) {
        if ($Line -match '^(MOSH_TEST_[A-Z_]+)=(.*)$') {
            $FixtureValues[$Matches[1]] = $Matches[2].Trim()
        }
    }
    foreach ($Name in @("MOSH_TEST_HOST", "MOSH_TEST_USER")) {
        if (-not $FixtureValues.ContainsKey($Name)) {
            throw "the WSL fixture did not report $Name"
        }
    }

    $HostAddress = $FixtureValues["MOSH_TEST_HOST"]
    $UserName = $FixtureValues["MOSH_TEST_USER"]
    $Target = "$UserName@$HostAddress"
    $CommonSshArguments = @(
        "-n",
        "-p", $SshPort,
        "-i", $PrivateKey,
        "-o", "IdentitiesOnly=yes",
        "-o", "BatchMode=yes",
        "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=NUL"
    )

    $SshSmokeOutput = & $SshExe @CommonSshArguments $Target -- sh -lc "command -v mosh-server >/dev/null && printf 'MOSH_SSH_OK\n'; sleep 1"
    $SshSmokeExit = $LASTEXITCODE
    if ($SshSmokeOutput -notcontains "MOSH_SSH_OK") {
        throw "Windows OpenSSH could not reach the temporary WSL sshd"
    }
    if ($SshSmokeExit -ne 0) {
        Write-Warning "Windows OpenSSH returned $SshSmokeExit after the remote success marker (Win32-OpenSSH issue #1899)"
    }
    Write-Host "MOSH_SSH_OK"

    if (-not $SshOnly) {
        $MoshArguments = @(
            "--server-address=$HostAddress",
            "--ssh-option=-p",
            "--ssh-option=$SshPort",
            "--ssh-option=-i",
            "--ssh-option=$PrivateKey",
            "--ssh-option=-o",
            "--ssh-option=IdentitiesOnly=yes",
            "--ssh-option=-o",
            "--ssh-option=BatchMode=yes",
            "--ssh-option=-o",
            "--ssh-option=ConnectTimeout=10",
            "--ssh-option=-o",
            "--ssh-option=StrictHostKeyChecking=no",
            "--ssh-option=-o",
            "--ssh-option=UserKnownHostsFile=NUL",
            "-p", $UdpPort,
            $Target,
            "sh", "-lc", "printf 'MOSH_NATIVE_E2E_OK\n'; sleep 1"
        )
        & $MoshExe @MoshArguments
        if ($LASTEXITCODE -ne 0) {
            throw "native Mosh WSL smoke test failed with exit code $LASTEXITCODE"
        }
    }

    Write-Host "WSL interoperability test passed for $Target"
}
finally {
    if ($FixtureStarted -and -not $KeepFixture) {
        & wsl.exe -d $Distro -u root -- bash $FixtureWslPath stop | Out-Null
    }
    if (-not $KeepFixture) {
        Remove-Item -LiteralPath $StateDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}

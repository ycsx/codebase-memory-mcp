<#
.SYNOPSIS
    Smoke-test a shipped Inno Setup installer in an isolated ASCII-only root.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer
)

$ErrorActionPreference = "Stop"
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$tempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$testRoot = Join-Path $tempBase ("cbm-installer-smoke-{0}-{1}" -f $PID, [guid]::NewGuid().ToString("N"))
$testRoot = [System.IO.Path]::GetFullPath($testRoot)
if (-not $testRoot.StartsWith($tempBase, [System.StringComparison]::OrdinalIgnoreCase) -or
    $testRoot -notmatch '^[\x00-\x7F]+$') {
    throw "Refusing installer smoke outside an ASCII temporary root: $testRoot"
}

$appDir = Join-Path $testRoot "app"
$cacheDir = Join-Path $testRoot "cache"
$profileDir = Join-Path $testRoot "profile"
$tempDir = Join-Path $testRoot "temp"
$consoleProcess = $null

try {
    New-Item -ItemType Directory -Path $appDir, $cacheDir, $profileDir, $tempDir -Force | Out-Null
    $env:HOME = $profileDir
    $env:USERPROFILE = $profileDir
    $env:LOCALAPPDATA = Join-Path $profileDir "AppData\Local"
    $env:TEMP = $tempDir
    $env:TMP = $tempDir
    $env:TMPDIR = $tempDir
    $env:CBM_CACHE_DIR = $cacheDir
    $env:CBM_INDEX_SUPERVISOR = "0"

    $configDb = Join-Path $cacheDir "_config.db"
    $indexDb = Join-Path $cacheDir "installer-smoke.db"
    Set-Content -LiteralPath $configDb -Value "preserve-config" -NoNewline
    Set-Content -LiteralPath $indexDb -Value "preserve-index" -NoNewline

    $installLog = Join-Path $testRoot "install.log"
    $installProcess = Start-Process -FilePath $installerPath -ArgumentList @(
        "/VERYSILENT",
        "/SUPPRESSMSGBOXES",
        "/NORESTART",
        "/NOICONS",
        "/NoUserPath=1",
        "/DIR=$appDir",
        "/LOG=$installLog"
    ) -Wait -PassThru -WindowStyle Hidden
    if ($installProcess.ExitCode -ne 0) {
        throw "installer failed with exit code $($installProcess.ExitCode)"
    }

    $binary = Join-Path $appDir "codebase-memory-mcp.exe"
    $desktopBinary = Join-Path $appDir "desktop\Codebase Memory.exe"
    $requiredFiles = @(
        $binary,
        $desktopBinary,
        (Join-Path $appDir "desktop\resources\app.asar"),
        (Join-Path $appDir "LICENSE"),
        (Join-Path $appDir "THIRD_PARTY_NOTICES.md"),
        (Join-Path $appDir "unins000.exe")
    )
    foreach ($required in $requiredFiles) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "installed payload is missing: $required"
        }
    }
    $duplicateBinary = Join-Path $appDir "desktop\resources\bin\codebase-memory-mcp.exe"
    if (Test-Path -LiteralPath $duplicateBinary) {
        throw "desktop payload contains a duplicate MCP binary"
    }

    & $binary --version
    if ($LASTEXITCODE -ne 0) { throw "installed binary failed --version" }

    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    $consoleProcess = Start-Process -FilePath $binary -ArgumentList @("console", "--no-open", "--port=$port") -PassThru -WindowStyle Hidden
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    $served = $false
    while ([DateTime]::UtcNow -lt $deadline -and -not $consoleProcess.HasExited) {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/" -TimeoutSec 1
            if ($response.StatusCode -eq 200 -and $response.Content.Length -gt 0) {
                $served = $true
                break
            }
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $served) { throw "installed console did not serve its embedded UI" }
    Stop-Process -Id $consoleProcess.Id -Force
    $consoleProcess.WaitForExit()
    $consoleProcess = $null

    $upgradeLog = Join-Path $testRoot "upgrade.log"
    $upgradeProcess = Start-Process -FilePath $installerPath -ArgumentList @(
        "/VERYSILENT",
        "/SUPPRESSMSGBOXES",
        "/NORESTART",
        "/NOICONS",
        "/NoUserPath=1",
        "/DIR=$appDir",
        "/LOG=$upgradeLog"
    ) -Wait -PassThru -WindowStyle Hidden
    if ($upgradeProcess.ExitCode -ne 0) {
        throw "in-place installer upgrade failed with exit code $($upgradeProcess.ExitCode)"
    }
    if ((Get-Content -Raw -LiteralPath $configDb) -ne "preserve-config") {
        throw "upgrade changed or removed _config.db"
    }
    if ((Get-Content -Raw -LiteralPath $indexDb) -ne "preserve-index") {
        throw "upgrade changed or removed project index data"
    }
    foreach ($required in $requiredFiles) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "upgraded payload is missing: $required"
        }
    }
    & $binary --version
    if ($LASTEXITCODE -ne 0) { throw "upgraded binary failed --version" }

    $uninstaller = Join-Path $appDir "unins000.exe"
    $uninstallLog = Join-Path $testRoot "uninstall.log"
    $uninstallProcess = Start-Process -FilePath $uninstaller -ArgumentList @(
        "/VERYSILENT",
        "/SUPPRESSMSGBOXES",
        "/NORESTART",
        "/LOG=$uninstallLog"
    ) -Wait -PassThru -WindowStyle Hidden
    if ($uninstallProcess.ExitCode -ne 0) {
        throw "uninstaller failed with exit code $($uninstallProcess.ExitCode)"
    }

    if ((Get-Content -Raw -LiteralPath $configDb) -ne "preserve-config") {
        throw "uninstall changed or removed _config.db"
    }
    if ((Get-Content -Raw -LiteralPath $indexDb) -ne "preserve-index") {
        throw "uninstall changed or removed project index data"
    }
    if (Test-Path -LiteralPath $binary) {
        throw "uninstall left the application binary behind"
    }
    Write-Host "Installer smoke passed; application removed and user data retained."
} finally {
    if ($consoleProcess -and -not $consoleProcess.HasExited) {
        Stop-Process -Id $consoleProcess.Id -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $testRoot) {
        $verifiedRoot = [System.IO.Path]::GetFullPath($testRoot)
        if ($verifiedRoot.StartsWith($tempBase, [System.StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $verifiedRoot).StartsWith("cbm-installer-smoke-")) {
            Remove-Item -LiteralPath $verifiedRoot -Recurse -Force
        }
    }
}

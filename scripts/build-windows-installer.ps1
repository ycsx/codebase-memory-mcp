<#
.SYNOPSIS
    Build a per-user Codebase Memory installer from a UI Windows binary.

.DESCRIPTION
    Stages an exact payload and invokes Inno Setup. The resulting installer
    contains the UI binary, desktop service controller, license, third-party
    notices, shortcuts, and the uninstaller. User indexes and configuration
    live outside the application directory and are deliberately retained on
    uninstall.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Binary,

    [Parameter(Mandatory = $true)]
    [string]$ThirdPartyNotices,

    [Parameter(Mandatory = $true)]
    [string]$DesktopPayload,

    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [ValidateSet("amd64", "arm64")]
    [string]$Architecture,

    [string]$OutputDirectory,
    [string]$IsccPath
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$installerSource = Join-Path $repoRoot "installer\windows\codebase-memory-mcp.iss"
$licenseSource = Join-Path $repoRoot "LICENSE"

function Resolve-RequiredFile {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-RequiredDirectory {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

$binaryPath = Resolve-RequiredFile $Binary "UI binary"
$noticesPath = Resolve-RequiredFile $ThirdPartyNotices "third-party notices"
$desktopPayloadPath = Resolve-RequiredDirectory $DesktopPayload "desktop payload"
$installerSource = Resolve-RequiredFile $installerSource "Inno Setup source"
$licenseSource = Resolve-RequiredFile $licenseSource "license"

$desktopExecutable = Join-Path $desktopPayloadPath "Codebase Memory.exe"
Resolve-RequiredFile $desktopExecutable "desktop executable" | Out-Null
$duplicateBinary = Join-Path $desktopPayloadPath "resources\bin\codebase-memory-mcp.exe"
if (Test-Path -LiteralPath $duplicateBinary -PathType Leaf) {
    throw "Desktop payload contains a duplicate MCP binary: $duplicateBinary"
}

$versionMatch = [regex]::Match($Version, '^v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:[-+][0-9A-Za-z.-]+)?$')
if (-not $versionMatch.Success) {
    throw "Version must be semantic (for example v0.9.0): $Version"
}
$appVersion = $Version -replace '^v', ''
$numericVersion = "$($versionMatch.Groups[1].Value).$($versionMatch.Groups[2].Value).$($versionMatch.Groups[3].Value).0"
$allowedArchitectures = if ($Architecture -eq "amd64") { "x64" } else { "arm64" }
$outputBaseFilename = "codebase-memory-mcp-ui-windows-$Architecture-setup"

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot "build\windows-installer\output"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$stageRoot = Join-Path $repoRoot "build\windows-installer\$Architecture"
$payloadDir = Join-Path $stageRoot "payload"
if (Test-Path -LiteralPath $payloadDir) {
    Remove-Item -LiteralPath $payloadDir -Recurse -Force
}
New-Item -ItemType Directory -Path $payloadDir -Force | Out-Null
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $payloadDir "codebase-memory-mcp.exe") -Force
Copy-Item -LiteralPath $noticesPath -Destination (Join-Path $payloadDir "THIRD_PARTY_NOTICES.md") -Force
Copy-Item -LiteralPath $licenseSource -Destination (Join-Path $payloadDir "LICENSE") -Force
$desktopStage = Join-Path $payloadDir "desktop"
New-Item -ItemType Directory -Path $desktopStage -Force | Out-Null
Copy-Item -Path (Join-Path $desktopPayloadPath "*") -Destination $desktopStage -Recurse -Force

if (-not $IsccPath) {
    $knownPaths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    )
    $IsccPath = $knownPaths | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $IsccPath) {
        $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($command) { $IsccPath = $command.Source }
    }
}
$IsccPath = Resolve-RequiredFile $IsccPath "Inno Setup compiler"

$expectedInstaller = Join-Path $OutputDirectory "$outputBaseFilename.exe"
if (Test-Path -LiteralPath $expectedInstaller) {
    Remove-Item -LiteralPath $expectedInstaller -Force
}

$arguments = @(
    "/Qp",
    "/DAppVersion=$appVersion",
    "/DNumericVersion=$numericVersion",
    "/DAllowedArchitectures=$allowedArchitectures",
    "/DOutputBaseFilename=$outputBaseFilename",
    "/DOutputDir=$OutputDirectory",
    "/DPayloadDir=$payloadDir",
    $installerSource
)
& $IsccPath @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $expectedInstaller -PathType Leaf)) {
    throw "Inno Setup did not produce the expected installer: $expectedInstaller"
}

$hash = (Get-FileHash -LiteralPath $expectedInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Built: $expectedInstaller"
Write-Host "SHA256: $hash"

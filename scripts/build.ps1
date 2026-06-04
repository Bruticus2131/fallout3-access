# Build wrapper for Fallout3Access.
# Reads FOSE_SDK_PATH and GAME_PATH from environment or from arguments.

[CmdletBinding()]
param(
    [string] $FoseSdkPath = $env:FOSE_SDK_PATH,
    [string] $GamePath    = $env:F3_GAME_PATH,
    [ValidateSet('Debug','Release','RelWithDebInfo')]
    [string] $Config = 'Release',
    [switch] $Deploy,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ($Clean -and (Test-Path 'build')) {
    Remove-Item -Recurse -Force build
}

if (-not $FoseSdkPath) {
    throw 'FOSE_SDK_PATH not provided. Pass -FoseSdkPath or set $env:FOSE_SDK_PATH.'
}
if (-not (Test-Path (Join-Path $FoseSdkPath 'fose\PluginAPI.h'))) {
    throw "FOSE SDK path '$FoseSdkPath' does not contain fose\PluginAPI.h."
}

$cmakeArgs = @(
    '-S', '.', '-B', 'build',
    '-G', 'Visual Studio 17 2022',
    '-A', 'Win32',
    "-DFOSE_SDK_PATH=$FoseSdkPath"
)
if ($GamePath) {
    $cmakeArgs += "-DGAME_PATH=$GamePath"
}

Write-Host "Configuring..." -ForegroundColor Cyan
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed.' }

Write-Host "Building $Config..." -ForegroundColor Cyan
& cmake --build build --config $Config
if ($LASTEXITCODE -ne 0) { throw 'cmake build failed.' }

if ($Deploy) {
    if (-not $GamePath) { throw 'Cannot deploy without -GamePath.' }
    Write-Host "Deploying..." -ForegroundColor Cyan
    & cmake --build build --config $Config --target deploy
    if ($LASTEXITCODE -ne 0) { throw 'deploy failed.' }
}

Write-Host "Done." -ForegroundColor Green

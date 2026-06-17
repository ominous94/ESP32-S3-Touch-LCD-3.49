param(
    [string]$ArduinoCli = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$sketchPath = Join-Path $projectRoot "Examples\Arduino\13_Launcher"
$buildPath = Join-Path $projectRoot ".arduino-build\13-launcher-16m-lvgl9-cdc-opi"
$lvglLibrary = Join-Path $projectRoot "Arduino_Libraries\lvgl9\lvgl"
$fqbn = "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,PSRAM=opi"

function Get-ArduinoCliPath {
    param([string]$PreferredPath)

    if ($PreferredPath) {
        if (-not (Test-Path -LiteralPath $PreferredPath)) {
            throw "Specified arduino-cli was not found: $PreferredPath"
        }
        return (Resolve-Path -LiteralPath $PreferredPath).Path
    }

    $command = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $defaultPath = Join-Path $env:LOCALAPPDATA "Programs\arduino-cli\arduino-cli.exe"
    if (Test-Path -LiteralPath $defaultPath) {
        return $defaultPath
    }

    throw "arduino-cli was not found. Install it or pass -ArduinoCli <path>."
}

$arduinoCliPath = Get-ArduinoCliPath -PreferredPath $ArduinoCli

if (-not (Test-Path -LiteralPath $lvglLibrary)) {
    throw "Required LVGL library root was not found: $lvglLibrary"
}

New-Item -ItemType Directory -Force -Path $buildPath | Out-Null

$arguments = @(
    "compile",
    "--build-path", $buildPath,
    "--library", $lvglLibrary,
    "--fqbn", $fqbn,
    $sketchPath
)

if ($Clean) {
    $arguments = @("compile", "--clean") + $arguments[1..($arguments.Length - 1)]
}

Write-Host "Arduino CLI: $arduinoCliPath"
Write-Host "Sketch: $sketchPath"
Write-Host "LVGL: $lvglLibrary"
Write-Host "FQBN: $fqbn"
Write-Host "Build path: $buildPath"
Write-Host ""

& $arduinoCliPath @arguments
exit $LASTEXITCODE

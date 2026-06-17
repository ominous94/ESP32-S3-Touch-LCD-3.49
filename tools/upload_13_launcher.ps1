param(
    [string]$ArduinoCli = "",
    [string]$Port = ""
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

function Get-Esp32Port {
    # Try to find an ESP32-S3 port
    $ports = Get-Port | Where-Object { $_.Description -like "*USB Serial*" -or $_.Description -like "*CDC*" -or $_.Description -like "*ESP32*" }
    if ($ports) {
        return $ports[0].Name
    }
    return ""
}

$arduinoCliPath = Get-ArduinoCliPath -PreferredPath $ArduinoCli

if (-not (Test-Path -LiteralPath $lvglLibrary)) {
    throw "Required LVGL library root was not found: $lvglLibrary"
}

# Find port if not specified
if (-not $Port) {
    Write-Host "Scanning for ports..."
    $portList = & $arduinoCliPath board list --format json | ConvertFrom-Json
    if ($portList -and $portList.Count -gt 0) {
        $espPort = $portList | Where-Object { $_.matching_boards -and $_.matching_boards.fqbn -like "*esp32*" } | Select-Object -First 1
        if ($espPort) {
            $Port = $espPort.port.address
            Write-Host "Found ESP32 board at: $Port"
        } else {
            # Just take the first port if no matching board
            $Port = $portList[0].port.address
            Write-Host "Using port: $Port"
        }
    }
}

if (-not $Port) {
    throw "No port found. Please connect your ESP32-S3 board, or specify port with -Port COMx"
}

Write-Host "Arduino CLI: $arduinoCliPath"
Write-Host "Sketch: $sketchPath"
Write-Host "LVGL: $lvglLibrary"
Write-Host "FQBN: $fqbn"
Write-Host "Port: $Port"
Write-Host "Build path: $buildPath"
Write-Host ""

# Check if build exists
if (-not (Test-Path -LiteralPath (Join-Path $buildPath "13_Launcher.ino.bin"))) {
    Write-Host "Build not found, compiling first..."
    $compileArgs = @(
        "compile",
        "--build-path", $buildPath,
        "--library", $lvglLibrary,
        "--fqbn", $fqbn,
        $sketchPath
    )
    & $arduinoCliPath @compileArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Compile failed"
    }
}

Write-Host "Uploading..."
$uploadArgs = @(
    "upload",
    "-p", $Port,
    "--fqbn", $fqbn,
    "--build-path", $buildPath,
    $sketchPath
)

& $arduinoCliPath @uploadArgs
exit $LASTEXITCODE

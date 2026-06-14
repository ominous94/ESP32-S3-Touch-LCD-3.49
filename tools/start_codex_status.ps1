param(
    [string]$HostAddress = "0.0.0.0",
    [int]$Port = 8787,
    [string]$SessionsFile = "",
    [string]$CodexHome = "",
    [int]$Limit = 5,
    [double]$Interval = 1.0,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
if (-not $SessionsFile) {
    $SessionsFile = Join-Path $projectRoot "sessions.json"
}

$logsDir = Join-Path $projectRoot "logs"
$exporterPidFile = Join-Path $logsDir "codex_status_exporter.pid"
$bridgePidFile = Join-Path $logsDir "codex_status_bridge.pid"
$exporterOutLog = Join-Path $logsDir "codex_status_exporter.out.log"
$exporterErrLog = Join-Path $logsDir "codex_status_exporter.err.log"
$bridgeOutLog = Join-Path $logsDir "codex_status_bridge.out.log"
$bridgeErrLog = Join-Path $logsDir "codex_status_bridge.err.log"

function Get-PythonCommand {
    $command = Get-Command python -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Python was not found on PATH. Install Python or add it to PATH before starting Codex Status services."
    }
    return $command.Source
}

function Test-ProcessIdRunning {
    param(
        [string]$PidFile,
        [string]$ExpectedScript
    )

    if (-not (Test-Path -LiteralPath $PidFile)) {
        return $false
    }

    $rawPid = (Get-Content -LiteralPath $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    $processId = 0
    if (-not [int]::TryParse($rawPid, [ref]$processId)) {
        return $false
    }

    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if (-not $process) {
        return $false
    }

    try {
        $commandLine = (Get-CimInstance Win32_Process -Filter "ProcessId = $processId" -ErrorAction Stop).CommandLine
    } catch {
        return $process.ProcessName -like "python*"
    }

    return $process.ProcessName -like "python*" -and $commandLine -like "*$ExpectedScript*"
}

function Start-CodexStatusProcess {
    param(
        [string]$Name,
        [string]$PidFile,
        [string]$OutLog,
        [string]$ErrLog,
        [string[]]$Arguments,
        [string]$ExpectedScript
    )

    if (Test-ProcessIdRunning -PidFile $PidFile -ExpectedScript $ExpectedScript) {
        $existingPid = Get-Content -LiteralPath $PidFile | Select-Object -First 1
        Write-Host "$Name is already running (PID $existingPid)."
        return
    }

    if ($DryRun) {
        Write-Host "[DryRun] python $($Arguments -join ' ')"
        return
    }

    $python = Get-PythonCommand
    $process = Start-Process -FilePath $python -ArgumentList $Arguments -WorkingDirectory $projectRoot -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog -WindowStyle Hidden -PassThru

    Set-Content -LiteralPath $PidFile -Value $process.Id -Encoding ASCII
    Write-Host "Started $Name (PID $($process.Id))."
}

function Get-LocalIPv4Addresses {
    $addresses = @()

    try {
        $addresses = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object {
                $_.IPAddress -ne "127.0.0.1" -and
                -not $_.IPAddress.StartsWith("169.254.") -and
                $_.PrefixOrigin -ne "WellKnown"
            } |
            Select-Object -ExpandProperty IPAddress -Unique
    } catch {
        $addresses = [System.Net.Dns]::GetHostEntry($env:COMPUTERNAME).AddressList |
            Where-Object { $_.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork } |
            ForEach-Object { $_.IPAddressToString } |
            Where-Object { $_ -ne "127.0.0.1" -and -not $_.StartsWith("169.254.") } |
            Select-Object -Unique
    }

    return @($addresses)
}

New-Item -ItemType Directory -Force -Path $logsDir | Out-Null

$exporterArgs = @(
    "tools\export_codex_sessions.py",
    "--output", $SessionsFile,
    "--limit", [string]$Limit,
    "--watch",
    "--interval", [string]$Interval
)
if ($CodexHome) {
    $exporterArgs += @("--codex-home", $CodexHome)
}

$bridgeArgs = @(
    "tools\codex_status_bridge.py",
    "--host", $HostAddress,
    "--port", [string]$Port,
    "--sessions-file", $SessionsFile
)

Write-Host "Starting Codex Status services from $projectRoot"
Write-Host "Sessions file: $SessionsFile"
Write-Host "Logs: $logsDir"
Write-Host ""

Start-CodexStatusProcess -Name "Codex session exporter" -PidFile $exporterPidFile -OutLog $exporterOutLog -ErrLog $exporterErrLog -Arguments $exporterArgs -ExpectedScript "tools\export_codex_sessions.py"
Start-CodexStatusProcess -Name "Codex status bridge" -PidFile $bridgePidFile -OutLog $bridgeOutLog -ErrLog $bridgeErrLog -Arguments $bridgeArgs -ExpectedScript "tools\codex_status_bridge.py"

Write-Host ""
Write-Host "Status endpoint:"
Write-Host "  http://127.0.0.1:$Port/status"
foreach ($address in Get-LocalIPv4Addresses) {
    Write-Host "  http://$address`:$Port/status"
}
Write-Host ""
Write-Host "Use one of the LAN URLs above for STATUS_URL on the ESP32."
Write-Host "To stop services later, stop the PIDs stored in logs\codex_status_*.pid."


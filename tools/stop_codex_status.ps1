param(
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$logsDir = Join-Path $projectRoot "logs"

function Get-ProcessTreeIds {
    param([int]$RootProcessId)

    $processes = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue)
    $result = [System.Collections.Generic.List[int]]::new()

    function Add-ChildrenFirst {
        param([int]$ParentProcessId)

        foreach ($child in $processes | Where-Object { $_.ParentProcessId -eq $ParentProcessId }) {
            Add-ChildrenFirst -ParentProcessId ([int]$child.ProcessId)
            $result.Add([int]$child.ProcessId)
        }
    }

    Add-ChildrenFirst -ParentProcessId $RootProcessId
    $result.Add($RootProcessId)
    return @($result)
}

function Stop-CodexStatusProcess {
    param(
        [string]$Name,
        [string]$PidFile,
        [string]$ExpectedScript
    )

    if (-not (Test-Path -LiteralPath $PidFile)) {
        Write-Host "$Name is not running (PID file is absent)."
        return $true
    }

    $rawPid = Get-Content -LiteralPath $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1
    $processId = 0
    if (-not [int]::TryParse($rawPid, [ref]$processId)) {
        Write-Warning "$Name has an invalid PID file; removing it: $PidFile"
        if (-not $DryRun) {
            Remove-Item -LiteralPath $PidFile -Force
        }
        return $true
    }

    $process = Get-CimInstance Win32_Process -Filter "ProcessId = $processId" -ErrorAction SilentlyContinue
    if (-not $process) {
        Write-Host "$Name is already stopped; removing the stale PID file."
        if (-not $DryRun) {
            Remove-Item -LiteralPath $PidFile -Force
        }
        return $true
    }

    $commandLine = [string]$process.CommandLine
    $isExpectedProcess = $process.Name -like "python*.exe" -and $commandLine -like "*$ExpectedScript*"
    if (-not $isExpectedProcess) {
        Write-Warning "$Name PID $processId belongs to another process. It will not be terminated; only the stale PID file will be removed."
        if (-not $DryRun) {
            Remove-Item -LiteralPath $PidFile -Force
        }
        return $true
    }

    $treeIds = @(Get-ProcessTreeIds -RootProcessId $processId)
    if ($DryRun) {
        Write-Host "[DryRun] Would stop $Name process tree: $($treeIds -join ', ')"
        return $true
    }

    foreach ($id in $treeIds) {
        Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    }

    Start-Sleep -Milliseconds 200
    $remaining = @($treeIds | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
    if ($remaining.Count -gt 0) {
        Write-Warning "$Name did not stop completely. Remaining PIDs: $($remaining -join ', ')"
        return $false
    }

    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    Write-Host "$Name stopped."
    return $true
}

$results = @(
    Stop-CodexStatusProcess `
        -Name "Codex App Server status adapter" `
        -PidFile (Join-Path $logsDir "codex_status_exporter.pid") `
        -ExpectedScript "tools\export_codex_sessions.py"
    Stop-CodexStatusProcess `
        -Name "Codex HTTP Bridge" `
        -PidFile (Join-Path $logsDir "codex_status_bridge.pid") `
        -ExpectedScript "tools\codex_status_bridge.py"
)

if ($results -contains $false) {
    exit 1
}

if ($DryRun) {
    Write-Host "Dry run completed; no processes were stopped."
} else {
    Write-Host "All Codex Status services are stopped."
}
exit 0

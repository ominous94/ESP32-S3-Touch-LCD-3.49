$portName = "COM9"
$baud = 115200
$seconds = 8
$logPath = "F:\CodexProject\ESP32-S3-Touch-LCD-3.49\logs\audio_test_serial.log"

$openDeadline = (Get-Date).AddSeconds(12)
$serial = $null

while ((Get-Date) -lt $openDeadline) {
    try {
        $serial = [System.IO.Ports.SerialPort]::new($portName, $baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $serial.ReadTimeout = 250
        $serial.DtrEnable = $false
        $serial.RtsEnable = $false
        $serial.Open()
        break
    } catch {
        if ($serial -ne $null) {
            $serial.Dispose()
            $serial = $null
        }
        Start-Sleep -Milliseconds 250
    }
}

if ($serial -eq $null -or -not $serial.IsOpen) {
    Write-Error "Could not open serial port $portName"
    exit 1
}

$utf8 = [System.Text.UTF8Encoding]::new($false)
$writer = [System.IO.StreamWriter]::new($logPath, $false, $utf8)
$deadline = (Get-Date).AddSeconds($seconds)
try {
    while ((Get-Date) -lt $deadline) {
        $text = $serial.ReadExisting()
        if ($text.Length -gt 0) {
            [Console]::Write($text)
            $writer.Write($text)
            $writer.Flush()
        }
        Start-Sleep -Milliseconds 100
    }
} finally {
    $writer.Dispose()
    $serial.Close()
    $serial.Dispose()
}

Write-Host "`n[log saved to $logPath]"

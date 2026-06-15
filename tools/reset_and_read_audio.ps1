$portName = "COM9"
$baud = 115200
$seconds = 10
$logPath = "F:\CodexProject\ESP32-S3-Touch-LCD-3.49\logs\audio_test_serial.log"

$serial = [System.IO.Ports.SerialPort]::new($portName, $baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 250
$serial.DtrEnable = $false
$serial.RtsEnable = $false

# Try to open the port, retrying briefly while a previous owner releases it
$opened = $false
for ($i = 0; $i -lt 20; $i++) {
    try {
        $serial.Open()
        $opened = $true
        break
    } catch {
        Start-Sleep -Milliseconds 200
    }
}
if (-not $opened) {
    Write-Error "Could not open $portName"
    exit 1
}
Write-Host "Opened $portName @ $baud"

# Toggle DTR/RTS to reset the ESP32-S3 (this mimics what esptool does)
$serial.DtrEnable = $true
$serial.RtsEnable = $false
Start-Sleep -Milliseconds 100
$serial.DtrEnable = $false
$serial.RtsEnable = $true
Start-Sleep -Milliseconds 100
$serial.DtrEnable = $false
$serial.RtsEnable = $false
Start-Sleep -Milliseconds 500

# Discard whatever might be in the buffer from the reset pulse
$serial.DiscardInBuffer()

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

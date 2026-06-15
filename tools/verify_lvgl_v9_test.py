"""抓 10_LVGL_V9_Test 烧录后的串口日志，做基本健康检查。"""
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOGS_DIR = ROOT / "logs"
PORT = "COM9"
BAUD = 115200
SECONDS = 10
STARTUP_WAIT = 4  # 烧录后等待设备重启稳定


def capture(port: str, baud: int, seconds: int, log_path: Path) -> int:
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    ps_script = r"""
$portName = $env:SERIAL_PORT
$baud = [int]$env:SERIAL_BAUD
$seconds = [int]$env:SERIAL_SECONDS
$logPath = $env:SERIAL_LOG
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
        if ($serial -ne $null) { $serial.Dispose(); $serial = $null }
        Start-Sleep -Milliseconds 250
    }
}
if ($serial -eq $null -or -not $serial.IsOpen) {
    throw "Could not open serial port $portName"
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
"""
    env = os.environ.copy()
    env.update({
        "SERIAL_PORT": port,
        "SERIAL_BAUD": str(baud),
        "SERIAL_SECONDS": str(seconds),
        "SERIAL_LOG": str(log_path.resolve()),
    })
    print(f"+ Waiting {STARTUP_WAIT}s for board boot, then capturing {seconds}s on {port} @ {baud}")
    import time
    time.sleep(STARTUP_WAIT)
    completed = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", ps_script],
        cwd=ROOT, text=True, env=env,
    )
    return completed.returncode


def evaluate(log_path: Path) -> int:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    print(f"\n=== Captured {len(text)} bytes ===")

    fail_tokens = [
        "assert failed", "Backtrace:", "Guru Meditation", "Rebooting",
        "Stack overflow", "panic", "abort()", "loadProhibited",
        "storeProhibited", "Main_Task_Overflow",
    ]
    hit_fail = [t for t in fail_tokens if t in text]
    lvgl_init_marker = "Initialize LVGL tick timer" in text or "Install LVGL tick timer" in text or "LVGL" in text
    spi_qspi_marker = "QSPI bus" in text or "axs15231b" in text or "axs" in text.lower()

    if hit_fail:
        print(f"FAIL: log contains dangerous tokens: {hit_fail}")
        return 1
    if not lvgl_init_marker:
        print("WARN: no LVGL init marker seen (board may not have started logging yet)")
    if not spi_qspi_marker:
        print("WARN: no QSPI init log seen (board may not have started logging yet)")
    print("OK: no assert / panic / reboot tokens")
    return 0


def main():
    log_path = LOGS_DIR / f"lvgl_v9_test_serial_{datetime.now().strftime('%Y%m%d-%H%M%S')}.log"
    rc = capture(PORT, BAUD, SECONDS, log_path)
    if rc != 0:
        print(f"Serial capture failed: exit {rc}")
        return rc
    print(f"\nLog file: {log_path}")
    return evaluate(log_path)


if __name__ == "__main__":
    raise SystemExit(main())

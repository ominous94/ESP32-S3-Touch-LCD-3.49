import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKETCH_PATH = ROOT / "Examples" / "Arduino" / "13_Launcher"
BUILD_PATH = ROOT / ".arduino-build" / "13-launcher-16m-lvgl9-cdc-opi"
COMPILE_SCRIPT = ROOT / "tools" / "compile_13_launcher.ps1"
LOGS_DIR = ROOT / "logs"
FQBN = "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,PSRAM=opi"
# 13_Launcher boots into the home screen; verifying the fix to the settings
# slider lag needs a human in the loop (drag a slider, watch the value label
# track the finger). This script is a smoke test: confirm the firmware boots
# and reaches the launcher home screen without crashing.
DEFAULT_REQUIRED_TOKENS = ["LAUNCHER boot", "LAUNCHER ready"]
DEFAULT_FORBIDDEN_TOKENS = [
    "assert failed",
    "Backtrace",
    "Rebooting",
    "RTC_SW_CPU_RST",
]


class VerificationError(RuntimeError):
    pass


@dataclass
class SerialLogResult:
    ok: bool
    missing_tokens: list
    forbidden_hits: list
    log_path: Path


def get_arduino_cli_path(preferred_path=""):
    if preferred_path:
        path = Path(preferred_path)
        if not path.is_file():
            raise VerificationError(f"Specified arduino-cli was not found: {preferred_path}")
        return str(path)

    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        default_path = Path(local_app_data) / "Programs" / "arduino-cli" / "arduino-cli.exe"
        if default_path.is_file():
            return str(default_path)

    return "arduino-cli"


def run_checked(command, cwd=ROOT):
    print("+ " + " ".join(str(part) for part in command))
    completed = subprocess.run(command, cwd=cwd, text=True)
    if completed.returncode != 0:
        raise VerificationError(f"Command failed with exit code {completed.returncode}")


def load_board_list(arduino_cli):
    completed = subprocess.run(
        [arduino_cli, "board", "list", "--format", "json"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        raise VerificationError(completed.stderr.strip() or "arduino-cli board list failed")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise VerificationError(f"Could not parse arduino-cli board list JSON: {exc}") from exc


def _port_address(entry):
    port = entry.get("port") if isinstance(entry, dict) else {}
    if not isinstance(port, dict):
        return ""
    return str(port.get("address") or "")


def _is_esp32_entry(entry):
    boards = entry.get("matching_boards") if isinstance(entry, dict) else []
    if not isinstance(boards, list):
        return False
    for board in boards:
        if not isinstance(board, dict):
            continue
        name = str(board.get("name") or "").lower()
        fqbn = str(board.get("fqbn") or "").lower()
        if "esp32" in name or fqbn.startswith("esp32:"):
            return True
    return False


def _is_usb_esp_vid(entry):
    port = entry.get("port") if isinstance(entry, dict) else {}
    props = port.get("properties") if isinstance(port, dict) else {}
    if not isinstance(props, dict):
        return False
    return str(props.get("vid") or "").lower() == "0x303a"


def choose_esp32_port(board_list):
    entries = board_list.get("detected_ports") if isinstance(board_list, dict) else []
    if not isinstance(entries, list):
        entries = []

    for entry in entries:
        address = _port_address(entry)
        if address and _is_esp32_entry(entry):
            return address

    for entry in entries:
        address = _port_address(entry)
        if address and _is_usb_esp_vid(entry):
            return address

    raise VerificationError("No ESP32 USB serial port found. Pass --port COMx explicitly.")


def default_log_path():
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return LOGS_DIR / f"launcher_serial_{stamp}.log"


def capture_serial_log_windows(port, baud, seconds, log_path):
    script = r"""
$portName = $env:CODEX_SERIAL_PORT
$baud = [int]$env:CODEX_SERIAL_BAUD
$seconds = [int]$env:CODEX_SERIAL_SECONDS
$logPath = $env:CODEX_SERIAL_LOG
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
    command = [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        script,
    ]
    print("+ powershell SerialPort capture")
    env = os.environ.copy()
    env.update(
        {
            "CODEX_SERIAL_PORT": port,
            "CODEX_SERIAL_BAUD": str(baud),
            "CODEX_SERIAL_SECONDS": str(seconds),
            "CODEX_SERIAL_LOG": str(Path(log_path).resolve()),
        }
    )
    completed = subprocess.run(command, cwd=ROOT, text=True, env=env)
    if completed.returncode != 0:
        raise VerificationError(f"Serial capture failed with exit code {completed.returncode}")


def capture_serial_log_with_monitor(arduino_cli, port, baud, seconds, log_path):
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    command = [
        arduino_cli,
        "monitor",
        "-p",
        port,
        "--config",
        f"baudrate={baud}",
        "--quiet",
    ]
    print("+ " + " ".join(command))
    deadline = time.monotonic() + seconds

    with Path(log_path).open("w", encoding="utf-8", errors="replace") as log_file:
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        try:
            while time.monotonic() < deadline:
                line = process.stdout.readline()
                if line:
                    print(line, end="")
                    log_file.write(line)
                    log_file.flush()
                elif process.poll() is not None:
                    break
                else:
                    time.sleep(0.05)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def capture_serial_log(arduino_cli, port, baud, seconds, log_path):
    LOGS_DIR.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        capture_serial_log_windows(port, baud, seconds, log_path)
    else:
        capture_serial_log_with_monitor(arduino_cli, port, baud, seconds, log_path)


def evaluate_serial_log(log_path, required_tokens, forbidden_tokens):
    text = Path(log_path).read_text(encoding="utf-8", errors="replace")
    missing = [token for token in required_tokens if token not in text]
    forbidden_hits = [token for token in forbidden_tokens if token in text]
    return SerialLogResult(
        ok=not missing and not forbidden_hits,
        missing_tokens=missing,
        forbidden_hits=forbidden_hits,
        log_path=Path(log_path),
    )


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Compile, optionally upload, and smoke-test 13_Launcher (LAUNCHER boot/ready + no crash signals)."
    )
    parser.add_argument("--arduino-cli", default="", help="Path to arduino-cli.exe.")
    parser.add_argument("--port", default="", help="Serial port such as COM9. Auto-detected by default.")
    parser.add_argument("--baud", default=115200, type=int)
    parser.add_argument("--monitor-seconds", default=15, type=int)
    parser.add_argument("--skip-compile", action="store_true")
    parser.add_argument("--skip-upload", action="store_true")
    parser.add_argument("--log-file", default="", help="Serial log output path.")
    parser.add_argument(
        "--require-token",
        action="append",
        default=[],
        help="Required substring in the captured serial log. May be passed multiple times.",
    )
    parser.add_argument(
        "--forbid-token",
        action="append",
        default=[],
        help="Forbidden substring; script fails if any are present. May be passed multiple times.",
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    arduino_cli = get_arduino_cli_path(args.arduino_cli)

    if args.port:
        port = args.port
    else:
        port = choose_esp32_port(load_board_list(arduino_cli))
    print(f"Serial port: {port}")

    if not args.skip_compile:
        run_checked(
            [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(COMPILE_SCRIPT),
                "-ArduinoCli",
                arduino_cli,
            ]
        )

    if not args.skip_upload:
        run_checked(
            [
                arduino_cli,
                "upload",
                "--input-dir",
                str(BUILD_PATH),
                "--fqbn",
                FQBN,
                "--port",
                port,
                str(SKETCH_PATH),
            ]
        )

    log_path = Path(args.log_file) if args.log_file else default_log_path()
    capture_serial_log(arduino_cli, port, args.baud, args.monitor_seconds, log_path)

    required_tokens = args.require_token or DEFAULT_REQUIRED_TOKENS
    forbidden_tokens = args.forbid_token or DEFAULT_FORBIDDEN_TOKENS
    result = evaluate_serial_log(log_path, required_tokens, forbidden_tokens)
    if not result.ok:
        if result.missing_tokens:
            print(f"Serial verification failed. Missing: {', '.join(result.missing_tokens)}")
        if result.forbidden_hits:
            print(f"Serial verification failed. Crash markers present: {', '.join(result.forbidden_hits)}")
        print(f"Serial log: {result.log_path}")
        return 1

    print("Serial verification passed.")
    print(f"Serial log: {result.log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

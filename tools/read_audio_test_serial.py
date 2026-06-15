"""Read 5 seconds of COM9 serial output via .NET SerialPort for Audio Test verification."""
import sys
import time
import clr

clr.AddReference("System")
clr.AddReference("System.IO.Ports")

from System.IO.Ports import SerialPort
from System import TimeSpan

COM_PORT = "COM9"
BAUD = 115200
DURATION = 6.0
LOG_PATH = r"F:\CodexProject\ESP32-S3-Touch-LCD-3.49\logs\audio_test_serial.log"

try:
    sp = SerialPort()
    sp.PortName = COM_PORT
    sp.BaudRate = BAUD
    sp.ReadTimeout = 200
    sp.Open()
    print(f"Opened {COM_PORT} @ {BAUD}")
    sp.DiscardInBuffer()

    lines = []
    start = time.time()
    while time.time() - start < DURATION:
        try:
            chunk = sp.ReadLine()
            chunk = chunk.rstrip("\r\n")
            stamp = time.strftime("%H:%M:%S")
            line = f"[{stamp}] {chunk}"
            print(line)
            lines.append(line)
        except Exception:
            # ReadTimeout or partial line; continue
            pass

    sp.Close()
    print(f"\nCaptured {len(lines)} lines in {DURATION}s")

    import os
    os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
    with open(LOG_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Saved log to: {LOG_PATH}")
except Exception as e:
    print(f"Error: {e}", file=sys.stderr)
    sys.exit(1)

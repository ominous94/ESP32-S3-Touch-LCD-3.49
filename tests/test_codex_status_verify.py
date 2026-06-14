import tempfile
import unittest
from pathlib import Path

from tools.verify_codex_status import (
    DEFAULT_REQUIRED_TOKENS,
    FQBN,
    VerificationError,
    choose_esp32_port,
    evaluate_serial_log,
)


ROOT = Path(__file__).resolve().parents[1]


class CodexStatusVerifyTests(unittest.TestCase):
    def test_choose_esp32_port_prefers_detected_esp32_usb_port(self):
        board_list = {
            "detected_ports": [
                {
                    "port": {
                        "address": "COM3",
                        "protocol_label": "Serial Port",
                        "properties": {},
                    }
                },
                {
                    "matching_boards": [
                        {"name": "ESP32 Family Device", "fqbn": "esp32:esp32:esp32_family"}
                    ],
                    "port": {
                        "address": "COM9",
                        "protocol_label": "Serial Port (USB)",
                        "properties": {"vid": "0x303A", "pid": "0x1001"},
                    },
                },
            ]
        }

        self.assertEqual(choose_esp32_port(board_list), "COM9")

    def test_evaluate_serial_log_accepts_required_codex_status_tokens(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "serial.log"
            log_path.write_text(
                "\n".join(
                    [
                        "CODEX_STATUS boot ms=51 heap=212000",
                        "CODEX_STATUS wifi_connecting ms=90 heap=211000",
                        "CODEX_STATUS ui_update connection=\"connected\" sessions=1",
                        "CODEX_STATUS http_ok code=200 bytes=120 sessions=1",
                    ]
                ),
                encoding="utf-8",
            )

            result = evaluate_serial_log(log_path, DEFAULT_REQUIRED_TOKENS)

        self.assertTrue(result.ok)
        self.assertEqual(result.missing_tokens, [])

    def test_evaluate_serial_log_reports_missing_tokens(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "serial.log"
            log_path.write_text("CODEX_STATUS boot\n", encoding="utf-8")

            result = evaluate_serial_log(log_path, ["CODEX_STATUS boot", "CODEX_STATUS ui_update"])

        self.assertFalse(result.ok)
        self.assertEqual(result.missing_tokens, ["CODEX_STATUS ui_update"])

    def test_choose_esp32_port_raises_when_no_candidate_exists(self):
        with self.assertRaises(VerificationError):
            choose_esp32_port({"detected_ports": [{"port": {"address": "COM3"}}]})

    def test_verify_upload_fqbn_enables_usb_cdc_serial_logs(self):
        self.assertIn("CDCOnBoot=cdc", FQBN)

    def test_verify_upload_fqbn_enables_opi_psram_for_lvgl_buffers(self):
        self.assertIn("PSRAM=opi", FQBN)

    def test_monitor_command_uses_explicit_baudrate_config(self):
        script = (ROOT / "tools" / "verify_codex_status.py").read_text(encoding="utf-8")

        self.assertIn("baudrate={baud}", script)

    def test_windows_serial_capture_uses_dotnet_serialport(self):
        script = (ROOT / "tools" / "verify_codex_status.py").read_text(encoding="utf-8")

        self.assertIn("System.IO.Ports.SerialPort", script)

    def test_cmd_verify_launcher_delegates_to_python_script(self):
        launcher = ROOT / "verify_codex_status.cmd"

        self.assertTrue(launcher.is_file(), "verify_codex_status.cmd is missing")
        text = launcher.read_text(encoding="utf-8")

        self.assertIn("tools\\verify_codex_status.py", text)
        self.assertIn("python", text.lower())
        self.assertIn("%*", text)

    def test_readme_documents_end_to_end_verification_command(self):
        readme = (ROOT / "README.md").read_text(encoding="utf-8")

        self.assertIn("verify_codex_status.cmd", readme)
        self.assertIn("--skip-upload", readme)
        self.assertIn("CODEX_STATUS", readme)


if __name__ == "__main__":
    unittest.main()

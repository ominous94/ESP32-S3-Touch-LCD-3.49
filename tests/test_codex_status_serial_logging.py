import unittest
from pathlib import Path


SKETCH_FILE = Path("Examples/Arduino/12_Codex_Status/12_Codex_Status.ino")


class CodexStatusSerialLoggingTests(unittest.TestCase):
    def test_sketch_emits_structured_serial_telemetry(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        expected_tokens = [
            "CODEX_STATUS boot",
            "CODEX_STATUS wifi_connecting",
            "CODEX_STATUS wifi_connected",
            "CODEX_STATUS wifi_failed",
            "CODEX_STATUS http_get",
            "CODEX_STATUS http_ok",
            "CODEX_STATUS http_error",
            "CODEX_STATUS ui_update",
            "CODEX_STATUS session",
        ]

        for token in expected_tokens:
            with self.subTest(token=token):
                self.assertIn(token, sketch)

    def test_status_updates_are_logged_after_display_updates(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("log_status_snapshot(connection, status);", sketch)
        self.assertIn("Serial.begin(115200);", sketch)
        self.assertIn("Serial.setTimeout(50);", sketch)


if __name__ == "__main__":
    unittest.main()

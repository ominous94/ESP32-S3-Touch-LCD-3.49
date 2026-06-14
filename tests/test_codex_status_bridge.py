import json
import socket
import threading
import time
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.request import urlopen

from tools.codex_status_bridge import StatusServer, build_status_payload


STATUS_TITLE = "\u72b6\u6001\u5c4f\u5f00\u53d1"
WORKING_ZH = "\u5de5\u4f5c\u4e2d"
DONE_ZH = "\u5df2\u5b8c\u6210"
UNTITLED_ZH = "\u672a\u547d\u540d\u4f1a\u8bdd"
DETAIL_TEXT = "\u7528\u6237\uff1a\u67e5\u770b\u5f53\u524d\u4f1a\u8bdd\u5185\u5bb9"


class CodexStatusBridgeTests(unittest.TestCase):
    def test_build_status_payload_defaults_to_empty_session_list(self):
        payload = build_status_payload()

        self.assertIn("updated_at", payload)
        self.assertIn("sessions", payload)
        self.assertEqual(payload["sessions"], [])

    def test_build_status_payload_reads_sessions_from_json_file(self):
        with TemporaryDirectory() as temp_dir:
            sessions_file = Path(temp_dir) / "sessions.json"
            sessions_file.write_text(
                json.dumps(
                    {
                        "updated_at": "2026-06-12 09:30:00",
                        "sessions": [
                            {
                                "title": STATUS_TITLE,
                                "state": "active",
                                "cwd": "ESP32-S3-Touch-LCD-3.49",
                                "updated_at": "2026-06-12 09:29:00",
                                "detail": DETAIL_TEXT,
                            },
                            {
                                "state": "complete",
                            },
                        ],
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )

            payload = build_status_payload(sessions_file=sessions_file)

        self.assertEqual(payload["updated_at"], "2026-06-12 09:30:00")
        self.assertEqual(len(payload["sessions"]), 2)

        first = payload["sessions"][0]
        self.assertEqual(first["title"], STATUS_TITLE)
        self.assertEqual(first["state"], "active")
        self.assertEqual(first["status_zh"], WORKING_ZH)
        self.assertEqual(first["cwd"], "ESP32-S3-Touch-LCD-3.49")
        self.assertEqual(first["detail"], DETAIL_TEXT)

        second = payload["sessions"][1]
        self.assertEqual(second["title"], UNTITLED_ZH)
        self.assertEqual(second["state"], "complete")
        self.assertEqual(second["status_zh"], DONE_ZH)
        self.assertEqual(second["cwd"], "")
        self.assertEqual(second["updated_at"], "2026-06-12 09:30:00")
        self.assertEqual(second["detail"], "")

    def test_missing_sessions_file_returns_empty_list(self):
        payload = build_status_payload(sessions_file="missing-sessions.json")

        self.assertIn("updated_at", payload)
        self.assertEqual(payload["sessions"], [])

    def test_status_endpoint_returns_json_payload_from_sessions_file(self):
        with TemporaryDirectory() as temp_dir:
            sessions_file = Path(temp_dir) / "sessions.json"
            sessions_file.write_text(
                json.dumps(
                    {
                        "sessions": [
                            {
                                "title": STATUS_TITLE,
                                "state": "active",
                                "status_zh": WORKING_ZH,
                                "cwd": "ESP32-S3-Touch-LCD-3.49",
                            }
                        ]
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )

            server = StatusServer(host="127.0.0.1", port=0, sessions_file=sessions_file)
            self.addCleanup(server.server_close)

            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()

            try:
                url = f"http://127.0.0.1:{server.port}/status"
                for _ in range(20):
                    try:
                        with urlopen(url, timeout=1) as response:
                            body = response.read().decode("utf-8")
                        break
                    except (OSError, socket.timeout):
                        time.sleep(0.05)
                else:
                    self.fail("status endpoint did not respond")

                payload = json.loads(body)
                self.assertIn("sessions", payload)
                self.assertEqual(payload["sessions"][0]["title"], STATUS_TITLE)
                self.assertEqual(payload["sessions"][0]["status_zh"], WORKING_ZH)
                self.assertIn(STATUS_TITLE, body)
                self.assertNotIn("\\u72b6", body)
            finally:
                server.shutdown()


if __name__ == "__main__":
    unittest.main()

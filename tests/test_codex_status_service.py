import json
import time
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.request import urlopen

from tools.codex_status_service import CodexStatusService


class FakeAppServer:
    def refresh(self, limit=20):
        return []

    def close(self):
        pass


class CodexStatusServiceTests(unittest.TestCase):
    def test_service_starts_bridge_and_exporter_then_stops(self):
        with TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            codex_home = root / ".codex"
            codex_home.mkdir()
            (codex_home / "session_index.jsonl").write_text(
                json.dumps(
                    {
                        "id": "thread-gui",
                        "thread_name": "GUI 服务测试",
                        "updated_at": "2026-06-18T01:00:00Z",
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-8",
            )
            sessions_file = root / "sessions.json"
            service = CodexStatusService(
                project_root=root,
                host="127.0.0.1",
                port=0,
                codex_home=codex_home,
                sessions_file=sessions_file,
                interval=0.05,
                app_server_factory=FakeAppServer,
            )

            service.start()
            try:
                self.assertTrue(service.is_running())
                for _ in range(30):
                    if sessions_file.is_file():
                        break
                    time.sleep(0.05)
                self.assertTrue(sessions_file.is_file())

                with urlopen(service.status_url, timeout=1) as response:
                    payload = json.loads(response.read().decode("utf-8"))

                self.assertEqual(payload["sessions"][0]["title"], "GUI 服务测试")
            finally:
                service.stop()

            self.assertFalse(service.is_running())


if __name__ == "__main__":
    unittest.main()

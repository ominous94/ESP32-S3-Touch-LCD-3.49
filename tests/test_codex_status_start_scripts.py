import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class CodexStatusStartScriptsTests(unittest.TestCase):
    def test_powershell_launcher_starts_exporter_and_bridge(self):
        script = ROOT / "tools" / "start_codex_status.ps1"

        self.assertTrue(script.is_file(), "tools/start_codex_status.ps1 is missing")
        text = script.read_text(encoding="utf-8")

        self.assertIn("export_codex_sessions.py", text)
        self.assertIn("codex_status_bridge.py", text)
        self.assertIn("--watch", text)
        self.assertIn("--sessions-file", text)
        self.assertIn("--host", text)
        self.assertIn("--port", text)
        self.assertIn("logs", text)
        self.assertIn("http://", text)
        self.assertIn("DryRun", text)

    def test_cmd_launcher_delegates_to_powershell_script(self):
        launcher = ROOT / "start_codex_status.cmd"

        self.assertTrue(launcher.is_file(), "start_codex_status.cmd is missing")
        text = launcher.read_text(encoding="utf-8")

        self.assertIn("powershell", text.lower())
        self.assertIn("tools\\start_codex_status.ps1", text)
        self.assertIn("-ExecutionPolicy", text)
        self.assertIn("%*", text)


if __name__ == "__main__":
    unittest.main()

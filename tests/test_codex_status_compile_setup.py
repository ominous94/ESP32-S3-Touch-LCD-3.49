import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class CodexStatusCompileSetupTests(unittest.TestCase):
    def test_powershell_compile_script_pins_lvgl9_and_16m_partition(self):
        script = ROOT / "tools" / "compile_codex_status.ps1"

        self.assertTrue(script.is_file(), "tools/compile_codex_status.ps1 is missing")
        text = script.read_text(encoding="utf-8")

        self.assertIn("arduino-cli.exe", text)
        self.assertIn("Arduino_Libraries\\lvgl9\\lvgl", text)
        self.assertIn("FlashSize=16M", text)
        self.assertIn("PartitionScheme=app3M_fat9M_16MB", text)
        self.assertIn("CDCOnBoot=cdc", text)
        self.assertIn("PSRAM=opi", text)
        self.assertIn("--build-path", text)
        self.assertIn("Examples\\Arduino\\12_Codex_Status", text)

    def test_cmd_compile_launcher_delegates_to_powershell_script(self):
        launcher = ROOT / "compile_codex_status.cmd"

        self.assertTrue(launcher.is_file(), "compile_codex_status.cmd is missing")
        text = launcher.read_text(encoding="utf-8")

        self.assertIn("powershell", text.lower())
        self.assertIn("tools\\compile_codex_status.ps1", text)
        self.assertIn("-ExecutionPolicy", text)
        self.assertIn("%*", text)


if __name__ == "__main__":
    unittest.main()

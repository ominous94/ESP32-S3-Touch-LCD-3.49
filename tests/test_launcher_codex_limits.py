import unittest
from pathlib import Path


APP_FILE = Path(
    "Examples/Arduino/13_Launcher/apps/codex_status/app_codex_status.cpp"
)
FONT_FILES = [
    Path("Examples/Arduino/13_Launcher/lv_font_codex_zh_16.c"),
    Path("Examples/Arduino/13_Launcher/lv_font_codex_zh_20.c"),
]


class LauncherCodexLimitsTests(unittest.TestCase):
    def test_firmware_parses_fixed_limit_fields(self):
        source = APP_FILE.read_text(encoding="utf-8")

        self.assertIn("struct CodexLimits", source)
        self.assertIn('json_int_value(json, "five_hour_used_percent", -1)', source)
        self.assertIn('json_int_value(json, "weekly_used_percent", -1)', source)
        self.assertIn('json_string_value(json, "five_hour_reset_text")', source)
        self.assertIn('json_string_value(json, "weekly_reset_text")', source)

    def test_primary_panel_displays_two_remaining_quota_bars(self):
        source = APP_FILE.read_text(encoding="utf-8")

        self.assertIn("lv_obj_t *limit_divider = lv_obj_create(pp);", source)
        self.assertIn("lv_obj_set_size(limit_divider, PRIMARY_TEXT_W, 1);", source)
        self.assertIn('const char *limit_names[2] = {"5小时余", "每周余"};', source)
        self.assertIn("limit_bars[i] = lv_bar_create(limit_row);", source)
        self.assertIn("int remaining_percent = 100 - window.used_percent;", source)
        self.assertIn('set_label_text(limit_value_labels[slot], "暂无");', source)
        self.assertIn("bind_limits_locked(status.limits);", source)

    def test_limit_telemetry_is_emitted(self):
        source = APP_FILE.read_text(encoding="utf-8")

        self.assertIn('log_serial_field("limits_status", status.limits.status);', source)
        self.assertIn('log_serial_field("five_hour_used"', source)
        self.assertIn('log_serial_field("weekly_used"', source)

    def test_embedded_fonts_cover_limit_labels(self):
        for font_file in FONT_FILES:
            source = font_file.read_text(encoding="utf-8")
            with self.subTest(font=font_file.name):
                for character in "小时每周余暂无":
                    self.assertIn(character, source)


if __name__ == "__main__":
    unittest.main()

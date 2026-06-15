import unittest
from pathlib import Path


SKETCH_DIR = Path("Examples/Arduino/12_Codex_Status")
LVGL_PORT_FILE = SKETCH_DIR / "lvgl_port.c"
CONFIG_FILE = SKETCH_DIR / "user_config.h"
LVGL_CONFIG_FILE = Path("Arduino_Libraries/lvgl9/lv_conf.h")


class CodexStatusScrollPerformanceTests(unittest.TestCase):
    def test_lvgl_refresh_period_stays_stable_for_full_frame_qspi_flush(self):
        config = LVGL_CONFIG_FILE.read_text(encoding="utf-8")

        self.assertIn("#define LV_DEF_REFR_PERIOD  33", config)

    def test_default_landscape_keeps_physical_qspi_write_window_stable(self):
        port = LVGL_PORT_FILE.read_text(encoding="utf-8")
        config = CONFIG_FILE.read_text(encoding="utf-8")

        self.assertIn("#define Rotated USER_DISP_ROT_90", config)
        self.assertIn("#define EXAMPLE_LCD_H_RES 172", config)
        self.assertIn("#define EXAMPLE_LCD_V_RES 640", config)
        self.assertIn("lv_draw_sw_rotate", port)
        self.assertIn("lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270)", port)
        self.assertIn("lvgl_dest", port)
        self.assertNotIn("esp_lcd_panel_swap_xy(panel, true)", port)
        self.assertNotIn("esp_lcd_panel_mirror(panel, false, true)", port)

    def test_display_keeps_qspi_full_frame_flush_to_avoid_panel_corruption(self):
        port = LVGL_PORT_FILE.read_text(encoding="utf-8")
        config = CONFIG_FILE.read_text(encoding="utf-8")

        self.assertIn("LV_DISPLAY_RENDER_MODE_FULL", port)
        self.assertIn("lv_display_set_buffers(disp, buffer_1, buffer_2, BUFF_SIZE", port)
        self.assertNotIn("LV_DISPLAY_RENDER_MODE_PARTIAL", port)
        self.assertNotIn("LVGL_PARTIAL_DRAW_BUFF_SIZE", config)
        self.assertNotIn("flush_lvgl_area_chunks", port)

    def test_touch_read_callback_does_not_log_on_the_hot_path(self):
        port = LVGL_PORT_FILE.read_text(encoding="utf-8")

        callback_start = port.index("static void TouchInputReadCallback")
        callback_end = port.index("static bool example_lvgl_lock", callback_start)
        callback_body = port[callback_start:callback_end]

        self.assertNotIn('ESP_LOGI("Touch"', callback_body)
        self.assertNotIn("ESP_LOGI(\"Touch\"", callback_body)


if __name__ == "__main__":
    unittest.main()

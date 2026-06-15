import unittest
from pathlib import Path


SKETCH_FILE = Path("Examples/Arduino/12_Codex_Status/12_Codex_Status.ino")
LVGL9_CONF_FILE = Path("Arduino_Libraries/lvgl9/lv_conf.h")


class CodexStatusSdTtfFontTests(unittest.TestCase):
    def test_lvgl_config_enables_file_backed_tiny_ttf(self):
        config = LVGL9_CONF_FILE.read_text(encoding="utf-8")

        self.assertIn("#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN", config)
        self.assertIn("#define LV_MEM_SIZE (512 * 1024U)", config)
        self.assertIn('#define LV_MEM_POOL_INCLUDE "esp_heap_caps.h"', config)
        self.assertIn("#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)", config)
        self.assertIn("#define LV_USE_TINY_TTF 1", config)
        self.assertIn("#define LV_TINY_TTF_FILE_SUPPORT 1", config)
        self.assertIn("#define LV_USE_FS_STDIO 1", config)
        self.assertIn("#define LV_FS_STDIO_LETTER 'S'", config)
        self.assertIn('#define LV_FS_STDIO_PATH "/sdcard"', config)

    def test_sketch_mounts_sd_card_and_loads_sd_ttf_by_default(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn('#include "esp_vfs_fat.h"', sketch)
        self.assertIn('#include "sdmmc_cmd.h"', sketch)
        self.assertNotIn("extern \"C\" void lv_fs_stdio_init(void);", sketch)
        self.assertNotIn("extern \"C\" lv_font_t *lv_tiny_ttf_create_file_ex", sketch)
        self.assertNotIn("lvgl/src/extra/libs", sketch)
        self.assertNotIn("src/extra/libs", sketch)
        self.assertIn('static const char *CODEX_STATUS_FONT_PATH = "S:/fonts/NotoSansSC-VF.ttf";', sketch)
        self.assertIn("#define CODEX_STATUS_ENABLE_SD_TTF 1", sketch)
        self.assertIn("init_sd_card();", sketch)
        self.assertIn("init_ttf_fonts();", sketch)
        self.assertIn("lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, LV_FONT_KERNING_NORMAL, 8192)", sketch)
        self.assertIn("lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, LV_FONT_KERNING_NORMAL, 8192)", sketch)

    def test_runtime_keeps_embedded_font_fallback_when_sd_ttf_cannot_load(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("#if CODEX_STATUS_ENABLE_SD_TTF && CODEX_STATUS_HAS_SD_TTF_SUPPORT", sketch)
        self.assertIn('Serial.print("CODEX_STATUS ttf_font_loaded path=");', sketch)
        self.assertIn('Serial.print("CODEX_STATUS ttf_font_fallback reason=create_failed path=");', sketch)
        self.assertIn('Serial.println("CODEX_STATUS ttf_font_fallback reason=sd_ttf_disabled");', sketch)

    def test_sketch_keeps_embedded_font_fallbacks(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("codex_font_16()", sketch)
        self.assertIn("codex_font_20()", sketch)
        self.assertIn("return ttf_font_16 != NULL ? ttf_font_16 : &lv_font_codex_zh_16;", sketch)
        self.assertIn("return ttf_font_20 != NULL ? ttf_font_20 : &lv_font_codex_zh_20;", sketch)

    def test_embedded_font_subset_covers_layout_a_text(self):
        font_16 = Path("Examples/Arduino/12_Codex_Status/lv_font_codex_zh_16.c").read_text(encoding="utf-8")
        font_20 = Path("Examples/Arduino/12_Codex_Status/lv_font_codex_zh_20.c").read_text(encoding="utf-8")
        required_text = "伙伴刷新秒下一步继续处理当前任务等待阻塞查看结果或收尾后暂无编译日志"

        for char in set(required_text):
            if char.isspace():
                continue
            with self.subTest(char=char):
                self.assertIn(char, font_16)
                self.assertIn(char, font_20)

    def test_sketch_uses_lvgl9_api_surface(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("LV_IMAGE_DECLARE", sketch)
        self.assertIn("lv_image_create", sketch)
        self.assertIn("lv_image_set_src", sketch)
        self.assertIn("lv_screen_active", sketch)


if __name__ == "__main__":
    unittest.main()

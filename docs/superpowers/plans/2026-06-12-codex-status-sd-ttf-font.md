# Codex Status SD TTF Font Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `Examples/Arduino/12_Codex_Status` use a Chinese TTF font file from SD card, while keeping the existing embedded LVGL fonts as a fallback.

**Architecture:** Mount the board SD card at `/sdcard`, register LVGL stdio FS as drive `S:`, create two LVGL tiny_ttf fonts from `S:/fonts/NotoSansSC-VF.ttf`, and route label font selection through helpers. If SD mount or TTF creation fails, helpers return the existing embedded subset fonts.

**Tech Stack:** Arduino ESP32, ESP-IDF SDMMC/VFS FAT, LVGL8 `tiny_ttf`, LVGL stdio FS driver, Python unittest static checks.

---

### Task 1: Add Static Tests

**Files:**
- Create: `tests/test_codex_status_sd_ttf_font.py`

- [ ] **Step 1: Write tests that expect SD TTF support**

```python
import unittest
from pathlib import Path


SKETCH_FILE = Path("Examples/Arduino/12_Codex_Status/12_Codex_Status.ino")
LV_CONF_FILE = Path("Arduino_Libraries/lvgl8/lv_conf.h")


class CodexStatusSdTtfFontTests(unittest.TestCase):
    def test_lvgl_config_enables_file_backed_tiny_ttf(self):
        config = LV_CONF_FILE.read_text(encoding="utf-8")

        self.assertIn("#define LV_USE_TINY_TTF 1", config)
        self.assertIn("#define LV_TINY_TTF_FILE_SUPPORT 1", config)
        self.assertIn("#define LV_USE_FS_STDIO 1", config)
        self.assertIn("#define LV_FS_STDIO_LETTER 'S'", config)
        self.assertIn('#define LV_FS_STDIO_PATH "/sdcard"', config)

    def test_sketch_mounts_sd_card_and_loads_ttf_fonts(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("#include \"esp_vfs_fat.h\"", sketch)
        self.assertIn("#include \"sdmmc_cmd.h\"", sketch)
        self.assertIn("#include \"lvgl/src/extra/libs/tiny_ttf/lv_tiny_ttf.h\"", sketch)
        self.assertIn("#include \"lvgl/src/extra/libs/fsdrv/lv_fsdrv.h\"", sketch)
        self.assertIn('static const char *CODEX_STATUS_FONT_PATH = "S:/fonts/NotoSansSC-VF.ttf";', sketch)
        self.assertIn("init_sd_card();", sketch)
        self.assertIn("init_ttf_fonts();", sketch)
        self.assertIn("lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16", sketch)
        self.assertIn("lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20", sketch)

    def test_sketch_keeps_embedded_font_fallbacks(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("codex_font_16()", sketch)
        self.assertIn("codex_font_20()", sketch)
        self.assertIn("return ttf_font_16 != NULL ? ttf_font_16 : &lv_font_codex_zh_16;", sketch)
        self.assertIn("return ttf_font_20 != NULL ? ttf_font_20 : &lv_font_codex_zh_20;", sketch)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and confirm it fails before implementation**

Run: `python -m unittest tests.test_codex_status_sd_ttf_font -v`

Expected: FAIL because the LVGL config and sketch do not yet contain SD-backed TTF support.

### Task 2: Implement SD-backed TTF Loading

**Files:**
- Modify: `Arduino_Libraries/lvgl8/lv_conf.h`
- Modify: `Examples/Arduino/12_Codex_Status/12_Codex_Status.ino`

- [ ] **Step 1: Enable LVGL tiny_ttf file loading**

Set `LV_USE_FS_STDIO`, `LV_USE_TINY_TTF`, and `LV_TINY_TTF_FILE_SUPPORT` to `1`. Set stdio letter to `S`, path to `/sdcard`, and a small cache.

- [ ] **Step 2: Add SD card mount and font helper functions**

Add ESP-IDF SDMMC includes, mount `/sdcard` on pins CLK 41, CMD 39, D0 40, initialize LVGL stdio FS, create 16/20 px TTF fonts from `S:/fonts/NotoSansSC-VF.ttf`, and expose `codex_font_16()`/`codex_font_20()` fallbacks.

- [ ] **Step 3: Route UI through helpers**

Change default label font and title/session title calls to use `codex_font_16()` and `codex_font_20()`.

- [ ] **Step 4: Initialize before UI creation**

Call `init_sd_card()` and `init_ttf_fonts()` after `lvgl_port_init()` and before `create_status_ui()`.

### Task 3: Verify

**Files:**
- Test: `tests/test_codex_status_sd_ttf_font.py`
- Test: existing Python tests

- [ ] **Step 1: Run focused test**

Run: `python -m unittest tests.test_codex_status_sd_ttf_font -v`

Expected: PASS.

- [ ] **Step 2: Run all Python tests**

Run: `python -m unittest discover -s tests -v`

Expected: PASS. Arduino firmware compile may still require Arduino IDE because `arduino-cli` is not available in this environment.

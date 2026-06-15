import re
import unittest
from pathlib import Path


SKETCH_DIR = Path("Examples/Arduino/12_Codex_Status")
IMAGE_DIR = SKETCH_DIR / "image"
GENERATED_IMAGE_FILE = SKETCH_DIR / "codex_status_images.c"
SKETCH_FILE = SKETCH_DIR / "12_Codex_Status.ino"
CONFIG_FILE = SKETCH_DIR / "user_config.h"


class CodexStatusImageTests(unittest.TestCase):
    def test_source_state_images_are_present(self):
        for state in ("work", "idle", "done", "error"):
            with self.subTest(state=state):
                self.assertTrue((IMAGE_DIR / f"{state}.png").is_file())

    def test_generated_lvgl_images_fill_right_landscape_panel(self):
        generated = GENERATED_IMAGE_FILE.read_text(encoding="utf-8")

        self.assertIn("LV_COLOR_FORMAT_RGB565", generated)
        for state in ("work", "idle", "done", "error"):
            with self.subTest(state=state):
                self.assertIn(f"codex_img_{state}", generated)
                match = re.search(
                    rf"codex_img_{state}\s*=\s*\{{.*?\.header\.w\s*=\s*(\d+),\s*"
                    rf".*?\.header\.h\s*=\s*(\d+),",
                    generated,
                    re.DOTALL,
                )
                self.assertIsNotNone(match)
                width, height = map(int, match.groups())
                self.assertEqual(width, 410)
                self.assertEqual(height, 160)

    def test_sketch_maps_codex_states_to_character_images(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        for state in ("work", "idle", "done", "error"):
            with self.subTest(state=state):
                self.assertIn(f"LV_IMAGE_DECLARE(codex_img_{state})", sketch)

        expected_pairs = {
            '"active"': "codex_img_work",
            '"complete"': "codex_img_done",
            '"blocked"': "codex_img_error",
            '"notLoaded"': "codex_img_idle",
        }
        for state, image_name in expected_pairs.items():
            with self.subTest(state=state):
                self.assertIn(state, sketch)
                self.assertIn(image_name, sketch)

        self.assertIn("lv_image_create", sketch)
        self.assertIn("lv_image_set_src", sketch)

    def test_sketch_uses_layout_a_companion_primary_secondary_columns(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")
        config = CONFIG_FILE.read_text(encoding="utf-8")

        self.assertIn("#define Rotated USER_DISP_ROT_90", config)
        self.assertIn("COMPANION_PANEL_W", sketch)
        self.assertIn("PRIMARY_PANEL_W", sketch)
        self.assertIn("SECONDARY_PANEL_W", sketch)
        self.assertIn("LV_FLEX_FLOW_ROW", sketch)
        self.assertIn("primary_title_label", sketch)
        self.assertNotIn("primary_next_label", sketch)
        self.assertNotIn("primary_verify_label", sketch)
        self.assertIn("secondary_cards", sketch)
        self.assertIn("assistant_image", sketch)
        self.assertNotIn("RIGHT_PANEL_W", sketch)
        self.assertIn("LV_FONT_DECLARE(lv_font_codex_zh_20)", sketch)
        self.assertIn("&lv_font_codex_zh_20", sketch)
        self.assertTrue((SKETCH_DIR / "lv_font_codex_zh_20.c").is_file())

    def test_layout_a_derives_primary_session_status_details(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("static int state_progress_width", sketch)
        self.assertNotIn("static String next_step_text", sketch)
        self.assertNotIn("下一步：", sketch)
        self.assertNotIn("验证：编译 + 真机日志", sketch)
        self.assertIn("暂无会话", sketch)

    def test_secondary_sessions_skip_primary_session(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("int session_index = order[i + 1];", sketch)
        self.assertNotIn("int session_index = order[i];", sketch)
        self.assertIn("static const int VISIBLE_SESSIONS = 5;", sketch)
        self.assertIn("static const int SECONDARY_SESSIONS = 4;", sketch)

    def test_layout_a_text_containers_are_fixed_and_non_scrollable(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("PRIMARY_TITLE_W", sketch)
        self.assertIn("SECONDARY_TITLE_W", sketch)
        self.assertIn("lv_obj_clear_flag(primary_panel, LV_OBJ_FLAG_SCROLLABLE)", sketch)
        self.assertIn("lv_obj_clear_flag(secondary_cards[i], LV_OBJ_FLAG_SCROLLABLE)", sketch)
        self.assertIn("lv_obj_add_flag(secondary_panel, LV_OBJ_FLAG_SCROLLABLE)", sketch)
        self.assertIn("lv_obj_set_scroll_dir(secondary_panel, LV_DIR_VER)", sketch)
        self.assertIn("lv_label_set_long_mode(primary_title_label, LV_LABEL_LONG_DOT)", sketch)
        self.assertIn("lv_label_set_long_mode(secondary_titles[i], LV_LABEL_LONG_DOT)", sketch)
        self.assertNotIn("lv_obj_set_height(session_rows[i], SESSION_ROW_H)", sketch)
        self.assertNotIn("lv_obj_set_height(session_rows[i], 35)", sketch)

    def test_sketch_adds_content_only_session_detail_page(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("String id;", sketch)
        self.assertIn('json_string_value(object_json, "id")', sketch)
        self.assertIn("String detail;", sketch)
        self.assertIn('json_string_value(object_json, "detail")', sketch)
        self.assertIn("enum CodexPage", sketch)
        self.assertIn("CODEX_PAGE_DETAIL", sketch)
        self.assertIn("detail_back_button", sketch)
        self.assertIn("detail_content_label", sketch)
        self.assertIn("open_session_detail", sketch)
        self.assertIn("return_to_home", sketch)
        self.assertIn("lv_obj_add_event_cb(primary_panel", sketch)
        self.assertIn("lv_obj_add_event_cb(secondary_cards[i]", sketch)
        self.assertIn("暂无会话内容", sketch)
        self.assertNotIn("detail_meta_label", sketch)
        self.assertNotIn("detail_status_label", sketch)

    def test_session_detail_clicks_do_not_deadlock_or_get_swallowed_by_children(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("render_detail_ui_locked", sketch)
        self.assertIn("render_home_ui_locked", sketch)
        self.assertIn("LV_OBJ_FLAG_EVENT_BUBBLE", sketch)
        self.assertIn("CODEX_STATUS detail_open", sketch)
        self.assertIn("mark_session_viewed", sketch)
        self.assertIn("/viewed?id=", sketch)
        self.assertIn("CODEX_STATUS detail_back", sketch)
        self.assertNotIn("create_detail_ui();\n}", sketch)
        self.assertNotIn("create_status_ui();\n  update_status_ui", sketch)

    def test_session_cards_have_pressed_and_released_visual_feedback(self):
        sketch = SKETCH_FILE.read_text(encoding="utf-8")

        self.assertIn("session_press_cancelled", sketch)
        self.assertIn("SESSION_DRAG_CANCEL_PX", sketch)
        self.assertIn("session_press_start", sketch)
        self.assertIn("back_press_cancelled", sketch)
        self.assertIn("session_feedback_event_cb", sketch)
        self.assertIn("detail_back_feedback_event_cb", sketch)
        self.assertIn("lv_event_get_current_target", sketch)
        self.assertIn("LV_EVENT_PRESSED", sketch)
        self.assertIn("LV_EVENT_RELEASED", sketch)
        self.assertIn("LV_EVENT_PRESSING", sketch)
        self.assertIn("LV_EVENT_PRESS_LOST", sketch)
        self.assertIn("session_pointer_inside", sketch)
        self.assertIn("lv_indev_get_point", sketch)
        self.assertIn("lv_obj_get_coords", sketch)
        self.assertIn("session_drag_exceeded", sketch)
        self.assertIn("session_press_cancelled[slot] = true;", sketch)
        self.assertIn("if (session_press_cancelled[slot]) return;", sketch)
        self.assertIn("if (!session_pointer_inside(target)) return;", sketch)
        self.assertIn("if (back_press_cancelled) return;", sketch)
        self.assertIn("lv_obj_add_event_cb(detail_back_button, return_to_home_event_cb, LV_EVENT_RELEASED", sketch)
        self.assertIn("set_card_pressed_feedback", sketch)
        self.assertIn("set_back_button_pressed_feedback", sketch)
        self.assertIn("lv_obj_set_size(detail_back_button, 68, 44);", sketch)
        self.assertIn("lv_obj_set_size(content_panel, DETAIL_CONTENT_W, PANEL_H);", sketch)
        self.assertIn("lv_obj_set_size(back_panel, DETAIL_BACK_PANEL_W, PANEL_H);", sketch)
        self.assertIn("lv_obj_create(screen);\n  lv_obj_set_size(content_panel", sketch)
        self.assertIn("lv_obj_create(screen);\n  lv_obj_set_size(back_panel", sketch)
        self.assertIn("lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);", sketch)
        self.assertIn("if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;", sketch)
        self.assertIn("lv_obj_add_event_cb(primary_panel, session_card_event_cb, LV_EVENT_RELEASED", sketch)
        self.assertIn("lv_obj_add_event_cb(secondary_cards[i], session_card_event_cb, LV_EVENT_RELEASED", sketch)
        self.assertIn("lv_obj_add_event_cb(primary_panel, session_feedback_event_cb, LV_EVENT_ALL", sketch)
        self.assertIn("lv_obj_add_event_cb(secondary_cards[i], session_feedback_event_cb, LV_EVENT_ALL", sketch)
        self.assertIn("lv_obj_add_event_cb(detail_back_button, detail_back_feedback_event_cb, LV_EVENT_ALL", sketch)


if __name__ == "__main__":
    unittest.main()

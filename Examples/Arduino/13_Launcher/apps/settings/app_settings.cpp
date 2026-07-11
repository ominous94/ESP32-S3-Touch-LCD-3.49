#include "app_settings.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "src/audio_bsp/audio_bsp.h"
#include "core/lv_obj_event_private.h"
#include <Preferences.h>

static Preferences s_prefs;
static const char *kPrefNamespace = "launcher";
static const char *kKeyBrightness = "brightness";
static const char *kKeyVolume = "volume";
static const int kMinBrightness = 128;  // 约 50%，限制滑条的最低亮度
static const int kDefaultBrightness = 255;
static const int kDefaultVolume = 50;

static lv_obj_t *g_scr = NULL;
static lv_obj_t *brightness_label = NULL;
static lv_obj_t *volume_label = NULL;
static lv_obj_t *brightness_slider = NULL;
static lv_obj_t *volume_slider = NULL;
static int g_brightness = kDefaultBrightness;
static int g_volume = kDefaultVolume;

static int s_last_brightness_duty = -1;
static int s_last_volume = -1;
static int s_last_brightness_pct = -1;

int settings_get_brightness(void) { return g_brightness; }
int settings_get_volume(void) { return g_volume; }

void settings_load(void) {
  s_prefs.begin(kPrefNamespace, true);
  g_brightness = s_prefs.getInt(kKeyBrightness, kDefaultBrightness);
  g_volume = s_prefs.getInt(kKeyVolume, kDefaultVolume);
  s_prefs.end();
  if (g_brightness < kMinBrightness) g_brightness = kMinBrightness;
  if (g_brightness > 255) g_brightness = 255;
  if (g_volume < 0) g_volume = 0;
  if (g_volume > 100) g_volume = 100;
}

static void settings_save_brightness(int val) {
  s_prefs.begin(kPrefNamespace, false);
  s_prefs.putInt(kKeyBrightness, val);
  s_prefs.end();
}

static void settings_save_volume(int val) {
  s_prefs.begin(kPrefNamespace, false);
  s_prefs.putInt(kKeyVolume, val);
  s_prefs.end();
}

static void back_cb(lv_event_t *e) {
  (void)e; launcher_request_return_home();
}

/* LVGL9 sliders only register touches on the knob by default
 * (see lv_slider.c LV_EVENT_HIT_TEST). On a 640x172 screen with a 10px-tall
 * track, the knob is too small to grab reliably, so the slider feels dead.
 * Overriding the hit test to accept the whole slider area gives the standard
 * "tap anywhere on the track" behaviour and matches the ESP slider demo. */
static void slider_hit_test_cb(lv_event_t *e) {
  lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
  if (info) info->res = true;
}

static void brightness_changed_cb(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  int val = (int)lv_slider_get_value(slider);
  uint16_t duty = (uint16_t)(255 - val);
  if ((int)duty == s_last_brightness_duty) return;
  s_last_brightness_duty = (int)duty;

  /* Always update backlight immediately for real-time response */
  setUpduty(duty);

  /* Only update label when displayed percentage actually changes */
  int pct = val * 100 / 255;
  if (pct != s_last_brightness_pct) {
    s_last_brightness_pct = pct;
    lv_color_t pct_color = lv_color_hex(val > 50 ? 0x78F0A4 : 0xF0C86F);
    if (val <= 20) pct_color = lv_color_hex(0xFF7E7E);
    char buf[16]; snprintf(buf, sizeof(buf), "%d%%", pct);
    if (brightness_label) {
      lv_label_set_text(brightness_label, buf);
      lv_obj_set_style_text_color(brightness_label, pct_color, 0);
    }
  }
}

static void volume_changed_cb(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  int val = (int)lv_slider_get_value(slider);
  if (val == s_last_volume) return;
  s_last_volume = val;
  g_volume = val;
  audio_bsp_set_volume(val);
  char buf[16]; snprintf(buf, sizeof(buf), "%d%%", val);
  if (volume_label) lv_label_set_text(volume_label, buf);
}

static void brightness_released_cb(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  int val = (int)lv_slider_get_value(slider);
  g_brightness = val;
  settings_save_brightness(val);
  int pct = val * 100 / 255;
  s_last_brightness_pct = pct;
  lv_color_t pct_color = lv_color_hex(val > 50 ? 0x78F0A4 : 0xF0C86F);
  if (val <= 20) pct_color = lv_color_hex(0xFF7E7E);
  char buf[16]; snprintf(buf, sizeof(buf), "%d%%", pct);
  if (brightness_label) {
    lv_label_set_text(brightness_label, buf);
    lv_obj_set_style_text_color(brightness_label, pct_color, 0);
  }
}

static void volume_released_cb(lv_event_t *e) {
  lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
  int val = (int)lv_slider_get_value(slider);
  g_volume = val;
  settings_save_volume(val);
}

/* Test button now plays the completion sound instead of flashing backlight.
 * The user can drag the volume slider, then tap this button to preview the
 * actual sound volume. */
static void play_test_tone_cb(lv_event_t *e) {
  (void)e;
  audio_bsp_play_complete_sound();
  Serial.println("CODEX_SETTINGS test_sound_triggered");
}

static void wifi_config_cb(lv_event_t *e) {
  (void)e;
  // 从 settings 内无法直接 switch（launcher_switch_to 开头 if(g_current_app!=NULL) return 会拒绝）。
  // 同时设 return_home + switch：launcher_process_pending 第一轮 destroy settings 回主页，
  // 第二轮（下个 loop 迭代）g_current_app 已 NULL，switch 成功进入 wifi_config。
  launcher_request_return_home();
  launcher_request_switch(app_idx::WIFI_CONFIG);
}

static lv_obj_t *build_setting_row(lv_obj_t *parent,
                                    const char *label_text,
                                    int range_min, int range_max, int initial,
                                    lv_color_t value_color,
                                    lv_obj_t **value_label_out,
                                    lv_event_cb_t value_changed_cb) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 26);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, label_text);
  lv_obj_set_style_text_font(lbl, codex_font_16(), 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xC6D1D8), 0);
  lv_obj_set_width(lbl, 48);

  lv_obj_t *slider = lv_slider_create(row);
  lv_obj_set_flex_grow(slider, 1);
  lv_obj_set_height(slider, 18);
  lv_obj_set_style_pad_left(slider, 6, 0);
  lv_obj_set_style_pad_right(slider, 6, 0);
  lv_slider_set_range(slider, range_min, range_max);
  lv_slider_set_value(slider, initial, LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, slider_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
  lv_obj_add_event_cb(slider, value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *value = lv_label_create(row);
  int pct_range = range_max > 0 ? range_max : 1;
  char buf[16]; snprintf(buf, sizeof(buf), "%d%%", initial * 100 / pct_range);
  lv_label_set_text(value, buf);
  lv_obj_set_style_text_font(value, codex_font_16(), 0);
  lv_obj_set_style_text_color(value, value_color, 0);
  lv_obj_set_width(value, 56);
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);

  if (value_label_out) *value_label_out = value;
  return slider;
}

lv_obj_t *app_settings_create(void) {
  g_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(g_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(g_scr, 12, 0);
  lv_obj_set_style_pad_row(g_scr, 8, 0);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(g_scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  /* Title */
  lv_obj_t *title = lv_label_create(g_scr);
  lv_label_set_text(title, "设置");
  lv_obj_set_style_text_font(title, codex_font_20(), 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF7FAFC), 0);

  /* Brightness row */
  brightness_slider = build_setting_row(g_scr, "亮度", kMinBrightness, 255, g_brightness,
                                          lv_color_hex(0x78F0A4),
                                          &brightness_label,
                                          brightness_changed_cb);
  lv_obj_add_event_cb(brightness_slider, brightness_released_cb, LV_EVENT_RELEASED, NULL);

  /* Volume row */
  volume_slider = build_setting_row(g_scr, "音量", 0, 100, g_volume,
                                      lv_color_hex(0x74B9FF),
                                      &volume_label,
                                      volume_changed_cb);
  lv_obj_add_event_cb(volume_slider, volume_released_cb, LV_EVENT_RELEASED, NULL);

  /* Bottom row: backlight test on the left, back on the right */
  lv_obj_t *bottom = lv_obj_create(g_scr);
  lv_obj_set_size(bottom, LV_PCT(100), 32);
  lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(bottom, 0, 0);
  lv_obj_set_style_pad_all(bottom, 0, 0);
  lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *test_btn = lv_btn_create(bottom);
  lv_obj_set_size(test_btn, 120, 30);
  lv_obj_set_style_bg_color(test_btn, lv_color_hex(0x1D3D5C), 0);
  lv_obj_set_style_radius(test_btn, 6, 0);
  lv_obj_set_style_border_width(test_btn, 0, 0);
  lv_obj_add_event_cb(test_btn, play_test_tone_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *tbl = lv_label_create(test_btn);
  lv_label_set_text(tbl, "测试提示音");
  lv_obj_center(tbl);
  lv_obj_set_style_text_font(tbl, codex_font_16(), 0);
  lv_obj_set_style_text_color(tbl, lv_color_hex(0x74B9FF), 0);

  lv_obj_t *wifi_btn = lv_btn_create(bottom);
  lv_obj_set_size(wifi_btn, 120, 30);
  lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(0x1D3D5C), 0);
  lv_obj_set_style_radius(wifi_btn, 6, 0);
  lv_obj_set_style_border_width(wifi_btn, 0, 0);
  lv_obj_add_event_cb(wifi_btn, wifi_config_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *wlbl = lv_label_create(wifi_btn);
  lv_label_set_text(wlbl, "WiFi 配网");
  lv_obj_center(wlbl);
  lv_obj_set_style_text_font(wlbl, codex_font_16(), 0);
  lv_obj_set_style_text_color(wlbl, lv_color_hex(0x74B9FF), 0);

  lv_obj_t *btn = lv_btn_create(bottom);
  lv_obj_set_size(btn, 100, 30);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, back_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "返回");
  lv_obj_center(lbl);
  lv_obj_set_style_text_font(lbl, codex_font_16(), 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xF7FAFC), 0);

  return g_scr;
}

void app_settings_destroy(lv_obj_t *scr) {
  (void)scr;
  brightness_label = NULL; volume_label = NULL;
  brightness_slider = NULL; volume_slider = NULL;
  s_last_brightness_duty = -1;
  s_last_brightness_pct = -1;
  s_last_volume = -1;
  g_scr = NULL;
}

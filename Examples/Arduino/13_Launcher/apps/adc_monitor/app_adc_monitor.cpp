#include "app_adc_monitor.h"

static void adc_back_cb(lv_event_t *e)
{
  (void)e;
  launcher_request_return_home();
}

lv_obj_t *app_adc_monitor_create(void)
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "ADC 监控");
  lv_obj_set_style_text_font(title, codex_font_20(), 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF7FAFC), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t *msg = lv_label_create(scr);
  lv_label_set_text(msg, "即将推出");
  lv_obj_set_style_text_font(msg, codex_font_16(), 0);
  lv_obj_set_style_text_color(msg, lv_color_hex(0x8FA1AD), 0);
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);

  lv_obj_t *btn = lv_btn_create(scr);
  lv_obj_set_size(btn, 100, 36);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, adc_back_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "返回");
  lv_obj_center(lbl);
  lv_obj_set_style_text_font(lbl, codex_font_16(), 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xF7FAFC), 0);

  return scr;
}

void app_adc_monitor_destroy(lv_obj_t *scr)
{
  (void)scr;
}

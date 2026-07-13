#include "apps.h"

static lv_obj_t *g_launcher_scr = NULL;
static lv_obj_t *g_current_app_scr = NULL;
static int g_current_app_index = -1;
static const LauncherApp *g_current_app = NULL;
static volatile int pending_app_switch = -1;
static volatile bool pending_return_home = false;

static lv_obj_t *g_bat_pct_label = NULL;
static lv_obj_t *g_wifi_label = NULL;
static lv_obj_t *g_bat_fill = NULL;
static lv_obj_t *g_bat_body = NULL;
static lv_obj_t *g_bat_terminal = NULL;
static lv_obj_t *g_bat_bolt = NULL;

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_color_t color, int width)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_width(label, width > 0 ? width : LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(label, codex_font_16(), 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  return label;
}

static void launcher_build_ui_locked(void)
{
  g_launcher_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(g_launcher_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(g_launcher_scr, 0, 0);
  lv_obj_clear_flag(g_launcher_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *status_bar = lv_obj_create(g_launcher_scr);
  lv_obj_set_size(status_bar, 640, 28);
  lv_obj_set_pos(status_bar, 0, 0);
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(status_bar, 0, 0);
  lv_obj_set_style_pad_all(status_bar, 0, 0);
  lv_obj_set_style_pad_left(status_bar, 8, 0);
  lv_obj_set_style_pad_right(status_bar, 8, 0);
  lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = create_label(status_bar, "启动器", lv_color_hex(0xF7FAFC), -1);
  lv_obj_set_style_text_font(title, codex_font_20(), 0);

  g_wifi_label = create_label(status_bar, "WiFi: --", lv_color_hex(0x8A99A5), 180);

  lv_obj_t *spacer = lv_obj_create(status_bar);
  lv_obj_set_size(spacer, 1, 1);
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);

  lv_obj_t *battery_icon = lv_obj_create(status_bar);
  lv_obj_set_size(battery_icon, 32, 16);
  lv_obj_clear_flag(battery_icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(battery_icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(battery_icon, 0, 0);
  lv_obj_set_style_pad_all(battery_icon, 0, 0);

  g_bat_body = lv_obj_create(battery_icon);
  lv_obj_set_size(g_bat_body, 28, 14);
  lv_obj_set_pos(g_bat_body, 0, 1);
  lv_obj_set_style_bg_opa(g_bat_body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_bat_body, 1, 0);
  lv_obj_set_style_border_color(g_bat_body, lv_color_hex(0xF7FAFC), 0);
  lv_obj_set_style_radius(g_bat_body, 3, 0);
  lv_obj_set_style_pad_all(g_bat_body, 2, 0);

  g_bat_fill = lv_obj_create(g_bat_body);
  lv_obj_set_size(g_bat_fill, 0, 10);
  lv_obj_set_pos(g_bat_fill, 0, 0);
  lv_obj_set_style_bg_color(g_bat_fill, lv_color_hex(0x78F0A4), 0);
  lv_obj_set_style_border_width(g_bat_fill, 0, 0);
  lv_obj_set_style_radius(g_bat_fill, 1, 0);

  g_bat_terminal = lv_obj_create(battery_icon);
  lv_obj_set_size(g_bat_terminal, 3, 6);
  lv_obj_set_pos(g_bat_terminal, 28, 5);
  lv_obj_set_style_bg_color(g_bat_terminal, lv_color_hex(0xF7FAFC), 0);
  lv_obj_set_style_border_width(g_bat_terminal, 0, 0);
  lv_obj_set_style_radius(g_bat_terminal, 1, 0);

  g_bat_bolt = lv_line_create(battery_icon);
  static lv_point_precise_t bolt_pts[] = {{9, 4}, {5, 8}, {10, 8}, {7, 12}};
  lv_line_set_points(g_bat_bolt, bolt_pts, 4);
  lv_obj_set_style_line_width(g_bat_bolt, 2, 0);
  lv_obj_set_style_line_color(g_bat_bolt, lv_color_hex(0xF0C86F), 0);
  lv_obj_set_style_line_rounded(g_bat_bolt, true, 0);
  lv_obj_add_flag(g_bat_bolt, LV_OBJ_FLAG_HIDDEN);

  g_bat_pct_label = create_label(status_bar, "--%", lv_color_hex(0xF7FAFC), 56);

  lv_obj_t *grid = lv_obj_create(g_launcher_scr);
  lv_obj_set_size(grid, 640, 144);
  lv_obj_set_pos(grid, 0, 28);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 4, 0);
  lv_obj_set_style_pad_column(grid, 16, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < g_app_count; i++) {
    const LauncherApp *app = &g_app_registry[i];
    if (app->hidden) continue;  // 隐藏的 app 不在主页网格显示（仍可由代码 switch 进入）

    lv_obj_t *cell = lv_obj_create(grid);
    lv_obj_set_size(cell, 112, 112);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1D252C), 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_radius(cell, 12, 0);
    lv_obj_set_style_shadow_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_add_event_cb(cell, [](lv_event_t *e) {
      int idx = (int)(intptr_t)lv_event_get_user_data(e);
      if (idx >= 0 && idx < g_app_count) launcher_request_switch(idx);
    }, LV_EVENT_RELEASED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(cell, [](lv_event_t *e) {
      lv_obj_t *c = (lv_obj_t *)lv_event_get_current_target(e);
      lv_obj_set_style_bg_color(c, lv_color_hex(0x2B3640), 0);
    }, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cell, [](lv_event_t *e) {
      lv_obj_t *c = (lv_obj_t *)lv_event_get_current_target(e);
      lv_obj_set_style_bg_color(c, lv_color_hex(0x1D252C), 0);
    }, LV_EVENT_RELEASED, NULL);

    if (app->icon_img != NULL) {
      lv_obj_t *img = lv_image_create(cell);
      lv_image_set_src(img, app->icon_img);
      lv_image_set_scale(img, app->icon_scale > 0 ? app->icon_scale : 128);
      lv_obj_set_style_image_recolor(img, lv_color_hex(0x74B9FF), 0);
      lv_obj_set_size(img, 48, 48);
      lv_obj_align(img, LV_ALIGN_CENTER, 0, -12);
    } else {
      lv_obj_t *char_label = lv_label_create(cell);
      lv_label_set_text(char_label, app->icon_char);
      lv_obj_set_style_text_font(char_label, codex_font_20(), 0);
      lv_obj_set_style_text_color(char_label, lv_color_hex(0x74B9FF), 0);
      lv_obj_align(char_label, LV_ALIGN_CENTER, 0, -12);
    }

    lv_obj_t *name_label = lv_label_create(cell);
    if (ttf_font_16 != NULL) {
      lv_label_set_text(name_label, app->name_zh);
    } else {
      lv_label_set_text(name_label, app->name_en);
    }
    lv_obj_set_style_text_font(name_label, codex_font_16(), 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xC6D1D8), 0);
    lv_obj_align(name_label, LV_ALIGN_CENTER, 0, 40);
  }
}

void launcher_init(void)
{
  // 启动时可以多等一下
  if (!lvgl_port_lock(1000)) return;
  launcher_build_ui_locked();
  if (g_launcher_scr != NULL) {
    lv_screen_load(g_launcher_scr);
  }
  lvgl_port_unlock();
}

void launcher_switch_to(int app_index)
{
  if (app_index < 0 || app_index >= g_app_count) return;
  if (g_current_app != NULL) return;

  const LauncherApp *app = &g_app_registry[app_index];

  // 只等待 500ms，而不是无限等待
  if (!lvgl_port_lock(500)) {
    Serial.println("LAUNCHER app_open lock timeout!");
    return;
  }

  lv_obj_t *app_scr = app->create();
  if (app_scr == NULL) {
    lvgl_port_unlock();
    return;
  }

  g_current_app_scr = app_scr;
  g_current_app_index = app_index;
  g_current_app = app;

  lv_screen_load_anim(app_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);

  lvgl_port_unlock();

  Serial.print("LAUNCHER app_open index=");
  Serial.print(app_index);
  Serial.print(" name=");
  Serial.println(app->name_zh);
}

void launcher_return_to_home(void)
{
  if (g_current_app == NULL) return;

  // 只等待 500ms，而不是无限等待
  if (!lvgl_port_lock(500)) {
    Serial.println("LAUNCHER app_close lock timeout!");
    // 返回请求来自 LVGL 事件任务，而页面切换在 Arduino loop 中执行。
    // 锁暂时繁忙时保留请求，避免用户必须再次点击。
    pending_return_home = true;
    return;
  }

  if (g_current_app->destroy != NULL) {
    g_current_app->destroy(g_current_app_scr);
  }

  lv_screen_load(g_launcher_scr);

  if (g_current_app_scr != NULL) {
    lv_obj_del(g_current_app_scr);
  }

  int closed_index = g_current_app_index;
  g_current_app_scr = NULL;
  g_current_app_index = -1;
  g_current_app = NULL;

  lvgl_port_unlock();

  Serial.print("LAUNCHER app_close index=");
  Serial.println(closed_index);
}

lv_obj_t *launcher_get_screen(void)
{
  return g_launcher_scr;
}

int launcher_current_app(void)
{
  return g_current_app_index;
}

void launcher_request_switch(int app_index)
{
  pending_app_switch = app_index;
}

void launcher_request_return_home(void)
{
  pending_return_home = true;
}

void launcher_process_pending(void)
{
  if (pending_return_home) {
    pending_return_home = false;
    if (g_current_app != NULL) {
      launcher_return_to_home();
      return;
    }
  }
  if (pending_app_switch >= 0) {
    int idx = pending_app_switch;
    pending_app_switch = -1;
    launcher_switch_to(idx);
  }
}

void launcher_tick_current_app(void)
{
  if (g_current_app != NULL && g_current_app->on_tick != NULL) {
    g_current_app->on_tick(g_current_app_scr);
  }
}

void launcher_update_wifi_name(const char *text, lv_color_t color) {
  if (g_wifi_label == NULL) return;
  if (lvgl_port_lock(200)) {
    lv_label_set_text(g_wifi_label, text);
    lv_obj_set_style_text_color(g_wifi_label, color, 0);
    lvgl_port_unlock();
  }
}

void launcher_update_battery(float volts, int pct, bool charging)
{
  if (g_bat_pct_label == NULL) return;
  lv_color_t color;
  if (pct < 20) color = lv_color_hex(0xFF7E7E);
  else if (pct < 60) color = lv_color_hex(0xF0C86F);
  else color = lv_color_hex(0x78F0A4);

  int fill_w = pct * 24 / 100;
  if (fill_w < 0) fill_w = 0;
  if (fill_w > 24) fill_w = 24;

  if (lvgl_port_lock(-1)) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(g_bat_pct_label, buf);
    lv_obj_set_style_text_color(g_bat_pct_label, color, 0);
    lv_obj_set_width(g_bat_fill, fill_w);
    lv_obj_set_style_bg_color(g_bat_fill, color, 0);
    lv_obj_set_style_border_color(g_bat_body, color, 0);
    lv_obj_set_style_bg_color(g_bat_terminal, color, 0);
    if (charging) {
      lv_obj_clear_flag(g_bat_bolt, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g_bat_bolt, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
  }
}

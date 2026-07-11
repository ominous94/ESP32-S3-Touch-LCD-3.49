#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include "lvgl_port.h"

#define LAUNCHER_APP_MAX 8

typedef struct {
  const char *name_zh;
  const char *name_en;
  const void *icon_img;
  const char *icon_char;
  lv_obj_t *(*create)(void);
  void (*destroy)(lv_obj_t *scr);
  void (*on_tick)(lv_obj_t *scr);
  bool hidden;  // true = 不在主页网格显示，但仍可通过 launcher_request_switch 进入
} LauncherApp;

extern const LauncherApp g_app_registry[];
extern const int g_app_count;

void launcher_init(void);
void launcher_request_switch(int app_index);
void launcher_request_return_home(void);
void launcher_process_pending(void);
void launcher_switch_to(int app_index);
void launcher_return_to_home(void);
lv_obj_t *launcher_get_screen(void);
int launcher_current_app(void);
void launcher_tick_current_app(void);
void launcher_update_battery(float volts, int pct, bool charging);
void launcher_update_wifi_name(const char *text, lv_color_t color);

extern lv_font_t *ttf_font_16;
extern lv_font_t *ttf_font_20;

const lv_font_t *codex_font_16(void);
const lv_font_t *codex_font_20(void);

// App indices — keep in sync with g_app_registry order in apps.h
// 放这里而非 apps.h，因为 apps.h 包含 g_app_registry 的定义，
// 被 .ino / launcher.cpp / apps.cpp 多次 include 会触发 multiple definition。
namespace app_idx {
  constexpr int CODEX_STATUS = 0;
  constexpr int IMU_TEST     = 1;
  constexpr int BALL_ROLL    = 2;
  constexpr int SETTINGS     = 3;
  constexpr int WIFI_CONFIG  = 4;
}

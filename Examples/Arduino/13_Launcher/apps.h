#pragma once

// App imports — each app is a self-contained project under apps/<name>/
// To add a new app: drop its folder, include its header here, add a line to the registry.
#include "apps/codex_status/app_codex_status.h"
#include "apps/imu_test/app_imu_test.h"
#include "apps/ball_roll/app_ball_roll.h"
#include "apps/ocean_water/app_ocean_water.h"
#include "apps/sd_browser/app_sd_browser.h"
#include "apps/settings/app_settings.h"
#include "apps/wifi_config/app_wifi_config.h"

// App indices — 定义放在 launcher.hpp，避免 apps.h 被 .ino/.cpp 多次 include 时重复定义 g_app_registry
const LauncherApp g_app_registry[] = {
  {
    .name_zh = "Codex 状态",
    .name_en = "Codex Status",
    .icon_img = &codex_img_work,
    .icon_char = "C",
    .create = app_codex_create,
    .destroy = app_codex_destroy,
    .on_tick = app_codex_tick,
  },
  {
    .name_zh = "IMU 测试",
    .name_en = "IMU Test",
    .icon_img = NULL,
    .icon_char = "I",
    .create = app_imu_test_create,
    .destroy = app_imu_test_destroy,
    .on_tick = app_imu_test_tick,
  },
  {
    .name_zh = "重力滚球",
    .name_en = "Ball Roll",
    .icon_img = NULL,
    .icon_char = "B",
    .create = app_ball_roll_create,
    .destroy = app_ball_roll_destroy,
    .on_tick = app_ball_roll_tick,
  },
  {
    .name_zh = "电子海洋",
    .name_en = "Ocean",
    .icon_img = NULL,
    .icon_char = "O",
    .create = app_ocean_water_create,
    .destroy = app_ocean_water_destroy,
    .on_tick = app_ocean_water_tick,
  },
  {
    .name_zh = "设置",
    .name_en = "Settings",
    .icon_img = NULL,
    .icon_char = "S",
    .create = app_settings_create,
    .destroy = app_settings_destroy,
    .on_tick = NULL,
  },
  {
    .name_zh = "WiFi 配网",
    .name_en = "WiFi Config",
    .icon_img = NULL,
    .icon_char = "W",
    .create = app_wifi_config_create,
    .destroy = app_wifi_config_destroy,
    .on_tick = app_wifi_config_tick,
    .hidden = true,
  },
};
const int g_app_count = sizeof(g_app_registry) / sizeof(g_app_registry[0]);

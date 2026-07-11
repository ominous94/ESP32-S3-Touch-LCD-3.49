#pragma once
#include "launcher.hpp"

// App lifecycle — 注册到 launcher，点击图标进入配网界面
lv_obj_t *app_wifi_config_create(void);
void app_wifi_config_destroy(lv_obj_t *scr);
void app_wifi_config_tick(lv_obj_t *scr);

// 给其他模块（codex_status / 主程序）用的接口
// 返回 NVS 里是否存有 WiFi 凭据
bool wifi_config_has_stored(void);
// 取出 NVS 凭据，写入 out_ssid / out_password（C 风格缓冲区，UTF-8）
// 返回 true 表示 NVS 有凭据且已写入；false 表示没有
bool wifi_config_get_stored(char *out_ssid, size_t ssid_cap,
                            char *out_password, size_t pwd_cap);

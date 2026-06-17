#pragma once
#include "launcher.hpp"
#include <HTTPClient.h>
#include <WiFi.h>

extern "C" {
LV_IMAGE_DECLARE(codex_img_work);
LV_IMAGE_DECLARE(codex_img_idle);
LV_IMAGE_DECLARE(codex_img_done);
LV_IMAGE_DECLARE(codex_img_error);
}

lv_obj_t *app_codex_create(void);
void app_codex_destroy(lv_obj_t *scr);
void app_codex_tick(lv_obj_t *scr);

void codex_connect_wifi(void);
bool codex_wifi_is_connected(void);

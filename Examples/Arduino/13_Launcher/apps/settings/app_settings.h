#pragma once
#include "launcher.hpp"

int settings_get_brightness(void);
int settings_get_volume(void);
void settings_load(void);
lv_obj_t *app_settings_create(void);
void app_settings_destroy(lv_obj_t *scr);

#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

/* 启用/关闭 native 方向的裁剪：仅刷新 y ∈ [0, y_max) 的区域 */
void lvgl_port_set_crop(bool enabled, int y_max);


#ifdef __cplusplus
}
#endif



#endif











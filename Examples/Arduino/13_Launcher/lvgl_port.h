#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

/* 启用/关闭 native 方向的裁剪：仅刷新 y ∈ [0, y_max) 的区域 */
void lvgl_port_set_crop(bool enabled, int y_max);

/*
 * 独占直绘模式：暂停 LVGL 刷新，由调用方把低分辨率 RGB565 网格直接展开到
 * 64 行 DMA 缓冲并连续写入面板。begin/end 必须在持有 lvgl_port_lock 时调用。
 */
bool lvgl_port_direct_mode_begin(void);
void lvgl_port_direct_mode_end(void);
bool lvgl_port_direct_draw_grid(const uint16_t *colors,
                                int grid_w, int grid_h,
                                int cell_size, int cell_draw_size,
                                int x_offset, uint32_t *elapsed_us);


#ifdef __cplusplus
}
#endif



#endif











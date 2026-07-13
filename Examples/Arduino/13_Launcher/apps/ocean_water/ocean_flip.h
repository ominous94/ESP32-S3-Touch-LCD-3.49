/*
 * 改编自“Bottle of Ocean”开源项目的 FLIP 水体模拟核心。
 * 原项目：https://oshwhub.com/hei_mao35/bottle_of_ocean
 * 许可证：GPL-3.0
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OceanFlipFluid OceanFlipFluid;

// 创建 FLIP 流体模拟器。
// - sim_w/sim_h: 模拟“水箱”物理尺寸（任意单位）
// - visible_w/visible_h: 需要输出到 LED 的可见网格分辨率（不含边界 padding）
// 注意：内部使用方格（统一 spacing），若 sim_w/sim_h 与 visible_w/visible_h 的比例不一致，
// 会在保证方格的前提下，将有效 tank 尺寸裁剪到可用范围。
OceanFlipFluid* ocean_flip_create(float sim_w, float sim_h, int visible_w, int visible_h, float fill_ratio);
void ocean_flip_destroy(OceanFlipFluid* f);

void ocean_flip_step(OceanFlipFluid* f, float dt, float gx, float gy);

// 在随设备旋转的二维参考系中推进模拟。
// - angular_velocity: 绕屏幕法向的角速度，单位 rad/s
// - angular_acceleration: 对应角加速度，单位 rad/s^2
// 求解器会逐粒子加入切向惯性、离心和科里奥利分量。
void ocean_flip_step_rotating(OceanFlipFluid* f, float dt, float gx, float gy,
                              float angular_velocity, float angular_acceleration);

// 在随机水面位置施加一次与重力垂直的局部速度冲击。
// - gravity_x/gravity_y: 当前二维重力方向
// - position: 沿水面切线方向的归一化位置（0.0 ~ 1.0）
// - direction: 小于 0 沿反切线方向，大于等于 0 沿正切线方向
// - strength: 冲击强度（常规建议 0.5 ~ 1.5，验证时可临时提高）
void ocean_flip_apply_wave_impulse(OceanFlipFluid* f, float gravity_x, float gravity_y,
                                   float position, float direction, float strength);

// 获取可见网格（布局为 out_grid[x * visible_h + y]）
void ocean_flip_get_led_grid(const OceanFlipFluid* f, float* out_grid, int visible_w, int visible_h);

// 根据潮位因子动态调整内部粒子数量：
// - tide_level: 0.0 = 最低潮, 1.0 = 最高潮
// - min_fill_ratio / max_fill_ratio: 相对于“基础水量”的下限/上限比例
//   （基础水量 = 初始创建时的粒子数）
void ocean_flip_set_tide_level(OceanFlipFluid* f, float tide_level,
                         float min_fill_ratio, float max_fill_ratio);

void ocean_flip_set_gravity_scale(OceanFlipFluid* f, float gravity_scale);
void ocean_flip_set_solver_quality(OceanFlipFluid* f, int push_iters, int pressure_iters, float ocean_flip_ratio);

#ifdef __cplusplus
}
#endif

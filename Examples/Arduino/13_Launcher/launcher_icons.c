#include "launcher_icons.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_LAUNCHER_ICON_OCEAN
#define LV_ATTRIBUTE_IMG_LAUNCHER_ICON_OCEAN
#endif

#define OCEAN_ICON_WIDTH 48
#define OCEAN_PIXEL(y, x) ((y) * OCEAN_ICON_WIDTH + (x))

/* 48 x 48 的 A8 遮罩：上方水滴，下方两层海浪。 */
static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST
LV_ATTRIBUTE_IMG_LAUNCHER_ICON_OCEAN uint8_t launcher_icon_ocean_map[48 * 48] = {
  /* 水滴 */
  [OCEAN_PIXEL(3, 23) ... OCEAN_PIXEL(3, 24)] = 0xFF,
  [OCEAN_PIXEL(4, 22) ... OCEAN_PIXEL(4, 25)] = 0xFF,
  [OCEAN_PIXEL(5, 21) ... OCEAN_PIXEL(5, 26)] = 0xFF,
  [OCEAN_PIXEL(6, 20) ... OCEAN_PIXEL(6, 27)] = 0xFF,
  [OCEAN_PIXEL(7, 19) ... OCEAN_PIXEL(7, 28)] = 0xFF,
  [OCEAN_PIXEL(8, 18) ... OCEAN_PIXEL(8, 29)] = 0xFF,
  [OCEAN_PIXEL(9, 17) ... OCEAN_PIXEL(9, 30)] = 0xFF,
  [OCEAN_PIXEL(10, 16) ... OCEAN_PIXEL(10, 31)] = 0xFF,
  [OCEAN_PIXEL(11, 15) ... OCEAN_PIXEL(11, 32)] = 0xFF,
  [OCEAN_PIXEL(12, 15) ... OCEAN_PIXEL(12, 32)] = 0xFF,
  [OCEAN_PIXEL(13, 16) ... OCEAN_PIXEL(13, 31)] = 0xFF,
  [OCEAN_PIXEL(14, 17) ... OCEAN_PIXEL(14, 30)] = 0xFF,
  [OCEAN_PIXEL(15, 19) ... OCEAN_PIXEL(15, 28)] = 0xFF,
  [OCEAN_PIXEL(16, 21) ... OCEAN_PIXEL(16, 26)] = 0xFF,

  /* 第一层海浪 */
  [OCEAN_PIXEL(21, 12) ... OCEAN_PIXEL(21, 19)] = 0xFF,
  [OCEAN_PIXEL(22, 9) ... OCEAN_PIXEL(22, 22)] = 0xFF,
  [OCEAN_PIXEL(22, 40) ... OCEAN_PIXEL(22, 43)] = 0xFF,
  [OCEAN_PIXEL(23, 7) ... OCEAN_PIXEL(23, 12)] = 0xFF,
  [OCEAN_PIXEL(23, 19) ... OCEAN_PIXEL(23, 24)] = 0xFF,
  [OCEAN_PIXEL(23, 37) ... OCEAN_PIXEL(23, 43)] = 0xFF,
  [OCEAN_PIXEL(24, 4) ... OCEAN_PIXEL(24, 10)] = 0xFF,
  [OCEAN_PIXEL(24, 22) ... OCEAN_PIXEL(24, 27)] = 0xFF,
  [OCEAN_PIXEL(24, 35) ... OCEAN_PIXEL(24, 41)] = 0xFF,
  [OCEAN_PIXEL(25, 4) ... OCEAN_PIXEL(25, 7)] = 0xFF,
  [OCEAN_PIXEL(25, 25) ... OCEAN_PIXEL(25, 30)] = 0xFF,
  [OCEAN_PIXEL(25, 32) ... OCEAN_PIXEL(25, 38)] = 0xFF,
  [OCEAN_PIXEL(26, 27) ... OCEAN_PIXEL(26, 36)] = 0xFF,

  /* 第二层海浪 */
  [OCEAN_PIXEL(33, 28) ... OCEAN_PIXEL(33, 35)] = 0xFF,
  [OCEAN_PIXEL(34, 25) ... OCEAN_PIXEL(34, 38)] = 0xFF,
  [OCEAN_PIXEL(34, 4) ... OCEAN_PIXEL(34, 7)] = 0xFF,
  [OCEAN_PIXEL(35, 4) ... OCEAN_PIXEL(35, 10)] = 0xFF,
  [OCEAN_PIXEL(35, 22) ... OCEAN_PIXEL(35, 27)] = 0xFF,
  [OCEAN_PIXEL(35, 35) ... OCEAN_PIXEL(35, 43)] = 0xFF,
  [OCEAN_PIXEL(36, 7) ... OCEAN_PIXEL(36, 13)] = 0xFF,
  [OCEAN_PIXEL(36, 19) ... OCEAN_PIXEL(36, 24)] = 0xFF,
  [OCEAN_PIXEL(36, 38) ... OCEAN_PIXEL(36, 43)] = 0xFF,
  [OCEAN_PIXEL(37, 10) ... OCEAN_PIXEL(37, 22)] = 0xFF,
  [OCEAN_PIXEL(37, 41) ... OCEAN_PIXEL(37, 43)] = 0xFF,
  [OCEAN_PIXEL(38, 13) ... OCEAN_PIXEL(38, 19)] = 0xFF,
};

const lv_image_dsc_t launcher_icon_ocean = {
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_A8,
  .header.flags = 0,
  .header.w = 48,
  .header.h = 48,
  .header.stride = 48,
  .data_size = sizeof(launcher_icon_ocean_map),
  .data = launcher_icon_ocean_map,
};

#undef OCEAN_PIXEL
#undef OCEAN_ICON_WIDTH

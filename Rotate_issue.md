# Rotate Issue — ESP 显示屏旋转画面的标准做法

> 调研对象：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT，基于 ESP-IDF + LVGL）
> 调研目的：弄清 ESP 系列开发板 LCD 旋转画面的标准实现，作为本项目（ESP32-S3-Touch-LCD-3.49）实现旋转功能的参考。
> 状态：调研已完成，方案已确定。本文档既是调研记录，也是未来实施时的依据。

---

## 1. 核心结论

ESP 屏旋转**不是一行配置能搞定的**。必须**双层同步**：

1. **物理层**：让 LCD 控制器（驱动芯片）按旋转后的方向扫描像素 → 调用 `esp_lcd_panel_swap_xy()` + `esp_lcd_panel_mirror()`。
2. **逻辑层**：让 LVGL 知道当前画布被旋转了 → 在 `lvgl_port_display_cfg_t.rotation`（ESP-IDF + lvgl_port）或 `lv_display_set_rotation()`（裸 LVGL 9）里设置同样的方向。

**两层用同一组 3 个 bool：`swap_xy` / `mirror_x` / `mirror_y`。** 只设其中任何一层，画面都会错。

---

## 2. 三个 bool 编码 4 种方向

| 方向 | swap_xy | mirror_x | mirror_y | 说明 |
|:---:|:---:|:---:|:---:|:---|
| 0°   | false | false | false | 自然方向 |
| 90°  | true  | true  | false | 顺时针 90° |
| 180° | false | true  | true  | 倒置 |
| 270° | true  | false | true  | 顺时针 270°（逆时针 90°） |

> 关键点：90° 和 270° 必然要 `swap_xy=true`（因为 X/Y 坐标系互换了），单独靠 mirror 只能做 0°↔180°。这是新手最常踩的坑。

---

## 3. xiaozhi-esp32 的架构

### 3.1 类层级

```
main/display/
├── display.h / display.cc              ← 抽象基类 Display
├── lcd_display.h / lcd_display.cc      ← LCD 抽象基类 LcdDisplay（继承 LvglDisplay）
│   ├── SpiLcdDisplay                   ← SPI 屏（ST7789/ILI9341/GC9A01 等）
│   ├── RgbLcdDisplay                   ← RGB 并口屏（GC9503 等）
│   └── MipiLcdDisplay                  ← MIPI-DSI 屏（ESP32-P4）
├── oled_display.h / oled_display.cc    ← OLED（SSD1306 等）
└── lvgl_display/                       ← LVGL 通用基类 + GIF/JPG/字体/主题
```

### 3.2 构造函数签名（三个具体 LCD 类完全一致）

```cpp
// lcd_display.h
SpiLcdDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel,
              uint16_t width, uint16_t height,
              uint16_t offset_x, uint16_t offset_y,
              bool mirror_x, bool mirror_y, bool swap_xy);
```

旋转相关的参数就是最后三个 bool。基类 `LcdDisplay` 把它们原样存到 `lvgl_port_display_cfg_t.rotation`：

```cpp
// lcd_display.cc
lvgl_port_display_cfg_t disp_cfg = {
    .io_handle   = panel_io,
    .panel_handle= panel,
    .buffer_size = width_ * 20,
    .hres        = width_,
    .vres        = height_,
    .rotation = {
        .swap_xy  = swap_xy,
        .mirror_x = mirror_x,
        .mirror_y = mirror_y,
    },
    .flags = {
        .buff_dma   = 1,    // SPI/RGB565 用 DMA
        .swap_bytes = 1,    // RGB565 字节序
        .sw_rotate  = 0,    // 优先硬件旋转
    },
};
lv_display_t *display = lvgl_port_add_disp(&disp_cfg);
```

### 3.3 三种 LCD 类的细微差异

| 特性 | SpiLcdDisplay | RgbLcdDisplay | MipiLcdDisplay |
|---|---|---|---|
| `double_buffer` | false | **true** | false |
| `full_refresh`  | 0     | **1**        | n/a  |
| `direct_mode`   | 0     | **1**        | n/a  |
| `sw_rotate`     | 0（硬件）| 0（硬件） | **true**（软件回写）|
| `avoid_tearing` | n/a   | 启用        | **禁用** |
| 缓冲行数        | 20 行 | 20 行       | **50 行** |
| 备注            | 标配 | 防撕裂 | DSI 控制器没硬件旋转，靠 LVGL 软件旋转 buffer |

> MIPI-DSI 用 `sw_rotate=true` 是性能 trade-off：CPU 帮 LVGL 旋转每一帧 buffer，**意味着屏幕方向在编译期定死、运行期不能改**——这也和 xiaozhi-esp32 整体"创建时一次设置"的哲学一致。

---

## 4. 双层旋转的具体代码（重点！）

### 4.1 SPI 屏的范式：sp-esp32-s3-1.54-muma

文件：`main/boards/sp-esp32-s3-1.54-muma/sp-esp32-s3-1.54-muma.cc`

```cpp
void InitializeSt7789Display() {
    // ... 创建 panel_io 和 panel ...
    esp_lcd_panel_init(panel);

    // ===== 第一层：物理旋转（驱动层） =====
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    // ===== 第二层：逻辑旋转（LVGL 层） =====
    // 同样的三个 bool 传给 LcdDisplay，让 LVGL 内部 buffer 与物理像素对齐
    display_ = new SpiLcdDisplay(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}
```

### 4.2 RGB 屏的范式：esp-s3-lcd-ev-board

文件：`main/boards/esp-s3-lcd-ev-board/esp-s3-lcd-ev-board.cc`

```cpp
void InitializeRGB_GC9503V_Display() {
    // ... 创建 RGB panel（GC9503，480x480）...
    esp_lcd_panel_init(panel_handle);

    display_ = new RgbLcdDisplay(panel_io, panel_handle,
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
}
```

> RGB 屏的 `mirror`/`swap_xy` 通常在 panel 驱动的初始化命令里完成（通过 SPI 发到 GC9503 的寄存器），所以这里只看到第二层。但本质和 SPI 屏一样——面板的物理方向和 LVGL 的逻辑方向必须一致。

### 4.3 为什么不能只做一层

| 只做哪一层 | 后果 |
|---|---|
| **只物理旋转**（esp_lcd_panel_*） | LCD 像素按新方向扫描，但 LVGL 仍按"自然方向"画坐标 → 画面旋转了，但**内容位置全错**（控件跑到屏外、文字倒着排） |
| **只逻辑旋转**（LVGL `.rotation`） | LVGL 画的内容旋转对了，但 ESP-LCD 驱动输出给屏的像素**还是原始方向** → 屏上看到的是"控件对了 + 物理没动" = 仍然错 |
| **两层同步** | 物理扫描方向 = LVGL 逻辑方向 → 像素与控件一一对应 ✓ |

### 4.4 偏移量是另一维度

旋转后画面可能需要平移（例如 1.54" 圆屏 240×240 但玻璃是 240×280，切掉一部分）。这个**不是旋转**，是逻辑坐标偏移：

```cpp
if (offset_x != 0 || offset_y != 0) {
    lv_display_set_offset(display_, offset_x, offset_y);
}
```

由 `DISPLAY_OFFSET_X/Y` 在 `config.h` 配置。

---

## 5. 板级 config.h 的标准格式

文件：`main/boards/<board>/config.h`

```cpp
// 屏尺寸
#define DISPLAY_WIDTH    480
#define DISPLAY_HEIGHT   480

// 旋转方向（3 个 bool 决定 4 种方向，详见第 2 节）
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

// 偏移（仅当玻璃比显示区域大需要裁切时使用）
#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0
```

> 改方向 = 改这 3 个宏。同一份 .cc 代码换板子时不用动，只换 config.h。

---

## 6. OLED 的简化路径

文件：`main/display/oled_display.cc`

```cpp
.rotation = {
    .swap_xy  = false,    // ← 写死 false
    .mirror_x = mirror_x,
    .mirror_y = mirror_y,
},
```

OLED 只支持左右/上下翻转，**没有 90° 旋转**。原因：OLED 几乎都是 128×64 这种横条形或 128×128 方形屏，旋转没意义。`monochrome = true` 是 OLED 专属。

---

## 7. 在本项目（ESP32-S3-Touch-LCD-3.49）的落地思路

本项目用的是 **Arduino + LVGL 9 + Arduino_GFX**（不是 ESP-IDF + lvgl_port），但**双层同步的原理完全一样**。落地方案如下：

### 7.1 物理层（Arduino_GFX）

`Arduino_GFX` 的 `GFX` 基类提供 `setRotation(uint8_t r)`，内部会调用具体面板驱动的 `setRotation`（ST7789/AXS15231B 等），效果等价于 ESP-IDF 的 `esp_lcd_panel_swap_xy/mirror`。例如：

```cpp
Arduino_GFX *gfx = new Arduino_AXS15231B(
    bus, DF_GFX_RST, 0 /* rotation */, false /* IPS */);
// 旋转 90°：
gfx->setRotation(1);   // 0/1/2/3 对应 0°/90°/180°/270°
```

### 7.2 逻辑层（LVGL 9）

在创建 `lv_display_t` 之后调用：

```cpp
lv_display_t *disp = lv_display_create(WIDTH, HEIGHT);
// ... 设 draw_buf、flush_cb ...
lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
```

> `LV_DISPLAY_ROTATION_0/90/180/270` 是 LVGL 9 的枚举，对应物理层 `setRotation(0/1/2/3)`。两边必须一致。

### 7.3 关键检查点

- [ ] Arduino_GFX 的 `setRotation()` 与 `lv_display_set_rotation()` 参数方向一致
- [ ] 如果用了 `lcd.setViewport()` / `lcd.setRotation()` 之外的"裁切"，对应的 LVGL 端用 `lv_display_set_offset()` 匹配
- [ ] 旋转后触摸坐标也要旋转——本项目用的是 AXS15231B 内建触摸，需要确认 `Arduino_GFX` 是否自动处理触摸旋转；若没有，需手动 `touch.setRotation()` 或在 LVGL indev 里做坐标变换
- [ ] 验证步骤：编译后用 `verify_codex_status.cmd` 跑一次，目视确认方向 + 触摸响应方向

---

## 8. 关键代码位置速查

| 想看什么 | 文件 | 关键行/区域 |
|---|---|---|
| 三个 bool 的最终消费处（LVGL 端） | `main/display/lcd_display.cc` | `SpiLcdDisplay/RgbLcdDisplay/MipiLcdDisplay` 构造函数的 `lvgl_port_display_cfg_t` |
| 三个 bool 的来源（板级） | `main/boards/<board>/<board>.cc` | `Initialize*Display()` 里 `new SpiLcdDisplay(...)` 的实参 |
| 三个 bool 的硬编码 | `main/boards/<board>/config.h` | `DISPLAY_MIRROR_X/Y` / `DISPLAY_SWAP_XY` |
| 板级物理旋转调用（SPI 屏） | `main/boards/sp-esp32-s3-1.54-muma/sp-esp32-s3-1.54-muma.cc` | `esp_lcd_panel_swap_xy` + `esp_lcd_panel_mirror` |
| 板级物理旋转调用（RGB 屏） | `main/boards/esp-s3-lcd-ev-board/esp-s3-lcd-ev-board.cc` | 通过 panel init 命令发到 GC9503 寄存器 |
| 偏移（与旋转正交） | `main/display/lcd_display.cc` | `lv_display_set_offset(display_, offset_x, offset_y)` |
| OLED 简化版 | `main/display/oled_display.cc` | `.rotation.swap_xy = false` |
| 官方添加新板指南 | `docs/custom-board.md` | "Apply Mirroring/Swap in the Board Class" 一节 |

---

## 9. 常见错误与排查

| 症状 | 可能原因 |
|---|---|
| 画面旋转 180° 但文字方向也反了 | 只设了 ESP-LCD 物理层，没设 LVGL `.rotation` |
| 画面看着是旋转了，但控件位置错乱（跑到屏外、文字重叠） | 同上 |
| 旋转后触摸不响应 / 方向错 | 触摸 IC 没跟着旋转；需要在触摸驱动里调 `setRotation()` 或 LVGL indev 里做坐标变换 |
| 旋转后画面只显示部分（左半边黑、右半边乱） | `DISPLAY_WIDTH/HEIGHT` 还是用旋转前的值；旋转 90°/270° 时宽高要对调 |
| 旋转后内容偏离中心 | `DISPLAY_OFFSET_X/Y` 没设对；或物理偏移（玻璃大于显示区）和逻辑偏移（`lv_display_set_offset`）不匹配 |
| 改完方向后白屏/无显示 | SPI 屏 `esp_lcd_panel_swap_xy/mirror` 顺序错了；某些面板必须先 swap_xy 再 mirror |
| RGB 屏旋转后撕裂 | `full_refresh`/`direct_mode` 标志没设（这是 `RgbLcdDisplay` 默认开的） |

---

## 10. 参考资料

- xiaozhi-esp32 仓库：https://github.com/78/xiaozhi-esp32
- 调研时的关键 commit：`main` 分支
- 官方"添加新板"文档：https://github.com/78/xiaozhi-esp32/blob/main/docs/custom-board.md
- LVGL 9 旋转 API 文档：`Arduino_Libraries/lvgl8/lvgl/docs/overview/display.md`（项目内）
- LVGL 8/9 旋转实现对比：本项目用的是 LVGL 9，API 名称是 `lv_display_set_rotation()`；老版 LVGL 8 是 `lv_disp_set_rotation()`

---

**最后更新**：2026-06-15
**作者**：Proma Agent 调研
**关联 issue**：本项目屏幕旋转功能

# ESP32-S3-Touch-LCD-3.49 项目知识库

## 编译配置（不可更改）
- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,PSRAM=opi`
- LVGL 库路径: `Arduino_Libraries/lvgl9/lvgl`（不要切到 lvgl8）
- 编译脚本: `tools/compile_13_launcher.ps1` / `compile_codex_status.cmd`

## 13_Launcher 的 app 编译机制（踩坑总结）

### apps.cpp 内联编译 → static 符号必须全局唯一
`apps.cpp` 用 `#include "apps/xxx/xxx.cpp"` 把所有 app 源码**内联到同一翻译单元**编译（Arduino 不自动编译子目录 .cpp 的变通）。
后果：各 app `.cpp` 里的 `static` 符号（变量、函数）实际处于同一作用域，**同名即 redefinition**。
- 反例：`app_settings.cpp` 和 `app_wifi_config.cpp` 都定义 `static const char *kPrefNamespace`、`static lv_obj_t *g_scr`、`static void back_cb(...)` → 编译报 redefinition
- 正解：每个 app 的 static 符号加模块前缀（如 `kWifiPrefNs`、`g_wifi_scr`、`wifi_back_cb`）

### apps.h 里的 g_app_registry 是定义不是声明 → 只能被一个 .cpp include
`apps.h` 直接写了 `const LauncherApp g_app_registry[] = {...}` 和 `const int g_app_count = ...`，是**定义**。
- `apps.h` 当前只被 `launcher.cpp` 直接 include（定义一次，OK）
- **绝不能**给 `apps.cpp` 内联的任何 app `.cpp` 加 `#include "apps.h"` —— 否则 `apps.cpp.o` 和 `launcher.cpp.o` 各有一份 `g_app_registry` 定义，链接报 multiple definition
- **绝不能**给 `13_Launcher.ino` 加 `#include "apps.h"` —— 同理，`.ino.cpp.o` 也会多一份定义
- 要在 `.ino` 或 app `.cpp` 里引用 app 编号，用 `launcher.hpp` 里的 `namespace app_idx`（constexpr 常量，声明性质，多处 include 安全）

### 添加新 app 的正确步骤
1. 建 `apps/<name>/app_<name>.h` + `.cpp`，`.cpp` 里 static 符号一律加 `<name>` 前缀
2. `apps.h` 顶部 `#include "apps/<name>/app_<name>.h"`
3. `apps.h` 的 `g_app_registry[]` 加一项，并在 `launcher.hpp` 的 `namespace app_idx` 加对应 constexpr
4. `apps.cpp` 末尾 `#include "apps/<name>/app_<name>.cpp"`
5. 需要从其他 app 跳转到新 app：`launcher_request_switch(app_idx::NEW_APP)`，不要 `#include "apps.h"`


## 已知问题

### 背光滑条不生效 [未解决]
- 13_Launcher 设置页的亮度滑条拖动流畅，值已持久化到 NVS，但**背光亮度实际不会变化**
- 已尝试的方法均无效：
  - LEDC PWM 控制 GPIO 8 → 无效
  - TCA9554 Pin 7 直接 toggle 高/低 → 无效
  - AW9364 脉冲控制 TCA9554 Pin 7 → 无效（拉高/拉低/发送脉冲序列均无响应）
- TCA9554 本身正常（Pin 6 电源保持功能工作正常）
- 可能原因：
  - V2 硬件变更了背光控制方式，Pin 7 可能不再控制背光
  - 背光可能连接在 TCA9554 的其他引脚（Pin 0-5）或 ESP32 的其他 GPIO
  - 背光可能由未发现的 I2C 芯片控制
- 下一步排查方向：
  - I2C 总线扫描，寻找未知设备
  - TCA9554 Pin 0-5 逐个 toggle 测试
  - GPIO 8 直接 `gpio_set_level()` toggle（非 LEDC PWM）
  - 查阅 V2 版本原理图
- 详细排查记录见 memory: [[backlight-fix-attempts]]

### AXS15231B QSPI 屏幕限制
- 不支持局部刷新，LVGL 必须用 FULL 渲染模式
- QSPI 频率上限 60MHz（80MHz 会撕裂），安全值 40-60MHz
- `esp_lcd_panel_swap_xy()` 对该驱动会导致黑屏，旋转用 LVGL 的 `lv_display_set_rotation()`

## 编码规则
- LVGL 回调中禁止 `Serial.print`（USB CDC 阻塞 5-30ms 会导致 UI 卡顿）
- 滑条回调做同值去重和百分比去重，减少不必要的全屏重绘
- TinyTTF 动态字体需要 `LV_MEM_SIZE >= 512K` + PSRAM heap 分配

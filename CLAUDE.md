# ESP32-S3-Touch-LCD-3.49 项目知识库

## 编译配置（不可更改）
- FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,PSRAM=opi`
- LVGL 库路径: `Arduino_Libraries/lvgl9/lvgl`（不要切到 lvgl8）
- 编译脚本: `tools/compile_13_launcher.ps1` / `compile_codex_status.cmd`

## 已知问题

### 背光滑条不生效 [未解决]
- 13_Launcher 设置页的亮度滑条拖动流畅，值已持久化到 NVS，但**背光亮度实际不会变化**
- GPIO 8（`EXAMPLE_PIN_NUM_BK_LIGHT`）直接 toggle 无效，TCA9554 Pin 7 toggle 也无效
- LEDC PWM API 调用成功但硬件无响应
- 可能原因：V2 硬件变更了背光控制方式，或背光通过其他芯片（如 AW9364）控制
- 详细排查记录见 `.context/note.md`

### AXS15231B QSPI 屏幕限制
- 不支持局部刷新，LVGL 必须用 FULL 渲染模式
- QSPI 频率上限 60MHz（80MHz 会撕裂），安全值 40-60MHz
- `esp_lcd_panel_swap_xy()` 对该驱动会导致黑屏，旋转用 LVGL 的 `lv_display_set_rotation()`

## 编码规则
- LVGL 回调中禁止 `Serial.print`（USB CDC 阻塞 5-30ms 会导致 UI 卡顿）
- 滑条回调做同值去重和百分比去重，减少不必要的全屏重绘
- TinyTTF 动态字体需要 `LV_MEM_SIZE >= 512K` + PSRAM heap 分配

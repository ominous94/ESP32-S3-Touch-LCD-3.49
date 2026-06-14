# 12_Codex_Status 编译报错记录

## 问题概述

执行 `compile_codex_status.cmd`（内部调用 `tools/compile_codex_status.ps1`）对
`Examples/Arduino/12_Codex_Status` 进行编译时，编译器抛出多个错误，编译失败。

## 实际错误信息

实际运行 `arduino-cli compile`（使用 `Arduino_Libraries\lvgl9\lvgl`，对应仓库的
默认编译配置）时报错如下：

```
F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status\12_Codex_Status.ino: In function 'void init_ttf_fonts()':
F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status\12_Codex_Status.ino:176:72: error: invalid conversion from 'int' to 'lv_font_kerning_t' [-fpermissive]
  176 |   ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, 8192);
      |                                                                        ^~~~
      |                                                                        |
      |                                                                        int
F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status\12_Codex_Status.ino:176:43: error: too few arguments to function 'lv_font_t* lv_tiny_ttf_create_file_ex(const char*, int32_t, lv_font_kerning_t, size_t)'
  176 |   ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, 8192);
      |                 ~~~~~~~~~~~~~~~~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
In file included from F:/CodexProject/ESP32-S3-Touch-LCD-3.49/Arduino_Libraries/lvgl9/lvgl/lvgl.h:116,
                 from F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Arduino_Libraries\lvgl9\lvgl\src/lvgl.h:16,
                 from F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status\12_Codex_Status.ino:15:
F:/CodexProject/ESP32-S3-Touch-LCD-3.49/Arduino_Libraries/lvgl9/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h:58:13: note: declared here
   58 | lv_font_t * lv_tiny_ttf_create_file_ex(const char * path, int32_t font_size, lv_font_kerning_t kerning,
      |             ^~~~~~~~~~~~~~~~~~~~~~~~~~
F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status\12_Codex_Status.ino:177:72: error: invalid conversion from 'int' to 'lv_font_kerning_t' [-fpermissive]
  177 |   ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, 8192);
      |                                                                        ^~~~
...
F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status\12_Codex_Status.ino:177:43: error: too few arguments to function 'lv_font_t* lv_tiny_ttf_create_file_ex(const char*, int32_t, lv_font_kerning_t, size_t)'
  177 |   ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, 8192);
      |                 ~~~~~~~~~~~~~~~~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
...
Error during build: exit status 1
```

仓库根目录旧的 `ErrorLog.txt` 里出现的一长串 `undefined reference to lv_mem_alloc / lv_mem_free / lv_lru_del`
是更早一次错误选择 `lvgl8` 路径（`Arduino_Libraries\lvgl8\lvgl`）构建时由链接器
输出的失败记录，与本次代码错误无关；当前仓库的默认编译脚本
`tools/compile_codex_status.ps1` 已经固定指向 `lvgl9`，不应再走 lvgl8 路径。

## 错误原因

仓库升级到 LVGL v9 后，`lv_tiny_ttf_create_file_ex()` 的函数签名增加了
`lv_font_kerning_t kerning` 参数。源码中调用方没有同步更新，仍然按旧的
3 参数形式调用，编译器因此报两类错误：

1. `invalid conversion from 'int' to 'lv_font_kerning_t'`：旧代码把数字
   `8192`（本意是 cache size）传给了现在类型为枚举 `lv_font_kerning_t` 的
   `kerning` 参数，类型不匹配。
2. `too few arguments ...`：函数现在有 4 个形参，调用只给了 3 个，实参数量不足。

`12_Codex_Status.ino` 顶部已经有 TTF 相关开关：

```cpp
#if LV_USE_FS_STDIO && LV_USE_TINY_TTF && LV_TINY_TTF_FILE_SUPPORT
#define CODEX_STATUS_USE_SD_TTF 1
#else
#define CODEX_STATUS_USE_SD_TTF 0
#endif
```

但本地 `lvgl9` 配置（`Arduino_Libraries\lvgl9\lvgl\src\lv_conf_internal.h`）
中 `LV_USE_TINY_TTF` 与 `LV_TINY_TTF_FILE_SUPPORT` 默认为开启，所以
`#if CODEX_STATUS_USE_SD_TTF` 这段会被编译进目标文件，触发了上面的类型错误。

### LVGL v9 中 `lv_tiny_ttf_create_file_ex` 的正确签名

```c
// Arduino_Libraries/lvgl9/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h
lv_font_t * lv_tiny_ttf_create_file_ex(const char * path,
                                        int32_t font_size,
                                        lv_font_kerning_t kerning,
                                        size_t cache_size);
```

`lv_font_kerning_t` 的取值（来自 `lv_font.h`）：

```c
typedef enum {
    LV_FONT_KERNING_NORMAL,
    LV_FONT_KERNING_NONE,
} lv_font_kerning_t;
```

## 解决办法

补上缺失的 `kerning` 形参，使用 `LV_FONT_KERNING_NORMAL` 保持原有渲染效果，
`cache_size` 仍为 8192。修改 `Examples/Arduino/12_Codex_Status/12_Codex_Status.ino`
中 `init_ttf_fonts()` 的两行调用：

```diff
- ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, 8192);
- ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, 8192);
+ ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, LV_FONT_KERNING_NORMAL, 8192);
+ ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, LV_FONT_KERNING_NORMAL, 8192);
```

## 验证

修复后重新执行：

```powershell
.\compile_codex_status.cmd
```

编译成功，最终输出与仓库 `AGENT.md` 中记录的“已验证编译结果”一致：

```
Sketch uses 1932533 bytes (61%) of program storage space. Maximum is 3145728 bytes.
Global variables use 113492 bytes (34%) of dynamic memory, leaving 214188 bytes for local variables. Maximum is 327680 bytes.
```

固件产出在 `.arduino-build/codex-status-16m-lvgl9/12_Codex_Status.ino.bin`，
可以通过 `start_codex_status.cmd` / `esptool` 烧录到设备。

## 经验教训

- 升级 LVGL（v8 → v9）时一定要以 `lv_tiny_ttf.h`、`lv_fs_stdio.h` 为入口，
  重新检查 `create_file_ex` / `create_data_ex` 等带 `_ex` 后缀的 API 签名变化。
- `lv_font_kerning_t` 并不是数值“越大越紧”，应当显式传枚举
  `LV_FONT_KERNING_NORMAL` / `LV_FONT_KERNING_NONE`，避免和 `cache_size`
  这样的 `size_t` 数值混淆。
- 仓库根目录的 `ErrorLog.txt` 历史上混合了多次失败构建的输出，定位
  编译问题时建议直接重新跑一次 `compile_codex_status.cmd`，以本次新报错
  为准；旧 `undefined reference` 链路是更早切到 lvgl8 编译路径时的产物，
  与当前 lvgl9 链路无关。

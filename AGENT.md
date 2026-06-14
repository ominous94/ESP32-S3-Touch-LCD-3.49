# Project Context

## Hardware

- Board model: ESP32-S3 Touch-LCD-3.49
- Display resolution: 172 x 640

## Development Notes

- Treat the display as a narrow portrait screen when designing UI layouts.
- Prefer compact status panels, vertical lists, and large touch targets that fit the 172 px width.
- Unless the user explicitly says otherwise, treat `Examples/Arduino/12_Codex_Status` as the default project to modify.

## Current Progress

- Added a minimum Codex status display example at `Examples/Arduino/12_Codex_Status`.
- Codex status project directory: `Examples/Arduino/12_Codex_Status` (`F:\CodexProject\ESP32-S3-Touch-LCD-3.49\Examples\Arduino\12_Codex_Status`).
- Installed `arduino-cli` locally and verified `esp32:esp32` core availability in this environment.
- The ESP32 sketch connects to Wi-Fi, polls `STATUS_URL`, parses a compact JSON response, and renders a Chinese multi-session status list.
- The current UI direction is the compact list layout: header, network/session summary, up to 5 session rows, and update time.
- Added a local Python bridge at `tools/codex_status_bridge.py`.
- Added a live session exporter at `tools/export_codex_sessions.py` and a combined launcher at `tools/start_codex_status.ps1`.
- The bridge reads exported Codex session data from `sessions.json` when started with `--sessions-file`, and the project launcher keeps that file refreshed automatically.
- Added bridge tests at `tests/test_codex_status_bridge.py`.
- Added project-specific LVGL Chinese fonts at `Examples/Arduino/12_Codex_Status/lv_font_codex_zh_16.c` and `Examples/Arduino/12_Codex_Status/lv_font_codex_zh_20.c`.
- The custom fonts were generated from `SourceHanSansSC-Normal.otf` and only include the Chinese characters currently used by the UI/session data, avoiding white-box glyphs while keeping firmware size lower.

## Runtime Setup

- Start the status services on the PC before using the board:

```powershell
.\start_codex_status.cmd
```

- The sketch currently uses:

```cpp
const char *STATUS_URL = "http://192.168.31.222:8787/status";
```

- If the PC IP changes, update `STATUS_URL` in `Examples/Arduino/12_Codex_Status/12_Codex_Status.ino`.
- The board and PC must be on the same LAN.

## Verification Status

- Python bridge tests pass with:

```powershell
python -m unittest tests.test_codex_status_bridge -v
```

- Python syntax checks passed.
- Static checks confirmed the current `.ino` Chinese characters are covered by the generated font files.
- `arduino-cli version` succeeds from this environment and reports `1.5.1`.
- `arduino-cli core list` shows `esp32:esp32 3.3.10` installed.
- Verified Arduino compile success requires the repo-local LVGL 9 library at `Arduino_Libraries/lvgl9/lvgl`, not a globally installed LVGL copy from the Arduino user libraries folder.
- Verified Arduino compile command:

```powershell
& "$env:LOCALAPPDATA\Programs\arduino-cli\arduino-cli.exe" compile --build-path .arduino-build\codex-status-16m-lvgl9-cdc-opi --library .\Arduino_Libraries\lvgl9\lvgl --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,PSRAM=opi .\Examples\Arduino\12_Codex_Status
```

- Verified compile result with USB CDC and OPI PSRAM enabled: program storage `1954400 / 3145728` bytes, global variables `114348 / 327680` bytes.
- Added a repo launcher at `tools/compile_codex_status.ps1` plus `compile_codex_status.cmd` to reproduce the verified compile settings.
- Added end-to-end verification at `tools/verify_codex_status.py` plus `verify_codex_status.cmd`.
- Verified serial route on `COM9`: upload succeeds, USB CDC serial logs are captured through Windows `.NET SerialPort`, and `CODEX_STATUS http_ok` / `CODEX_STATUS ui_update` are observed.
- Latest successful serial log: `logs/codex_status_serial_20260613-024124.log`.

## Mandatory Build/Deploy Verification Workflow

- When the user asks to compile, build, verify, or check firmware changes for `Examples/Arduino/12_Codex_Status`, do not stop at a successful local compile. The expected completion condition is: compile succeeds, firmware can be deployed to the connected ESP32-S3 board, serial logs can be captured, and the log contains successful `CODEX_STATUS` telemetry.
- Preferred full command:

```cmd
verify_codex_status.cmd
```

- This command uses `tools/verify_codex_status.py` to auto-detect the ESP32 USB serial port, compile with the verified FQBN, upload the firmware, capture serial output, and require `CODEX_STATUS http_` plus `CODEX_STATUS ui_update`.
- If the firmware is already compiled and uploaded, the lighter serial-only check is:

```cmd
verify_codex_status.cmd --skip-compile --skip-upload
```

- If compile, upload, or serial verification fails, use this same chain as the debugging loop:
  1. Read the failing command output and the generated `logs/codex_status_serial_*.log`.
  2. Identify whether the failure is compile-time, upload/port detection, boot/runtime assertion, Wi-Fi/HTTP, or missing telemetry.
  3. Fix the root cause in the sketch, scripts, or board options.
  4. Re-run `verify_codex_status.cmd` until it compiles, deploys, and passes serial telemetry checks.
- For this board, successful serial verification normally shows `CODEX_STATUS boot`, `CODEX_STATUS wifi_connected`, `CODEX_STATUS http_ok`, `CODEX_STATUS ui_update`, and one or more `CODEX_STATUS session` lines.
- Do not claim a firmware change is verified unless the relevant command has passed in the current session and the serial log confirms the expected `CODEX_STATUS` events.

## Compile Lessons

- Detailed compile record: `Examples/Arduino/12_Codex_Status/Issue_record.md`.
- Current LVGL path for this sketch is LVGL9: `Arduino_Libraries/lvgl9/lvgl`.
- Do not switch this sketch to `Arduino_Libraries/lvgl8/lvgl`; the UI code and image descriptors use LVGL9 APIs such as `lv_image_*`, `lv_display_*`, and `lv_screen_active()`.
- Keep `CDCOnBoot=cdc` in the Arduino FQBN; otherwise `Serial.print()` telemetry is not visible on the USB serial port used by this board.
- Keep `PSRAM=opi` in the Arduino FQBN; `lvgl_port_init()` allocates LVGL display buffers with `MALLOC_CAP_SPIRAM` and will assert on `buffer_1` when PSRAM is disabled.
- On Windows, `arduino-cli monitor` did not reliably capture this board's USB CDC output in this environment. Use the verification script's `.NET SerialPort` capture path instead.
- Do not diagnose current LVGL9 failures from the old root-level `ErrorLog.txt` alone. That file contains earlier failures from an `lvgl8` build path, including `undefined reference to lv_mem_alloc`, `lv_mem_free`, and `lv_lru_del`. Re-run `compile_codex_status.cmd` and use the fresh output.
- LVGL9 `lv_tiny_ttf_create_file_ex()` takes 4 arguments: `path`, `font_size`, `lv_font_kerning_t kerning`, and `cache_size`.
- The correct calls in `init_ttf_fonts()` are:

```cpp
ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, LV_FONT_KERNING_NORMAL, 8192);
ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, LV_FONT_KERNING_NORMAL, 8192);
```

- Do not revert those calls to the older 3-argument form. If compile output says `invalid conversion from 'int' to 'lv_font_kerning_t'` or `too few arguments`, the missing argument is `LV_FONT_KERNING_NORMAL`.
- The linker warning about `_floatdidf.o: missing .note.GNU-stack section` is a toolchain warning. It is not the same problem as TTF API argument mismatch and is not the compile blocker recorded in `Issue_record.md`.
- 动态中文字体必须同时满足 TinyTTF 开启和 LVGL heap 足够大。不要只把 `CODEX_STATUS_ENABLE_SD_TTF` 改成 `1`，否则 TinyTTF 会继续挤在默认 64KB LVGL 内置 heap 里，容易在 `stbtt__new_active` 断言后反复重启。
- 当前稳定配置是 `Arduino_Libraries/lvgl9/lv_conf.h` 中 `LV_MEM_SIZE (512 * 1024U)`，并通过 `LV_MEM_POOL_INCLUDE "esp_heap_caps.h"` 与 `LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 把 LVGL heap 放到 PSRAM。
- 验证动态字体时，串口日志必须出现 `CODEX_STATUS ttf_font_loaded`，并且多轮 `CODEX_STATUS http_ok` / `CODEX_STATUS ui_update` 后不能出现 `assert failed`、`stbtt__new_active`、`Backtrace`、`Rebooting`、`RTC_SW_CPU_RST`。
- 本次坑点已记录到 Obsidian：`F:\CodexProject\CodexVault\ESP32-S3-Touch-LCD-3.49\LVGL TinyTTF 动态字体内存坑点.md`。

## Known Limits

- The bridge depends on the local exporter process to keep `sessions.json` fresh; running `tools/codex_status_bridge.py` alone without `--sessions-file` returns an empty session list.
- The custom Chinese fonts are intentionally subsetted. If new Chinese words are added to the UI or exported session data, regenerate or extend `lv_font_codex_zh_16.c` and `lv_font_codex_zh_20.c`; otherwise LVGL may show white boxes for missing glyphs.
- The ESP32 JSON parser is a lightweight fixed-field parser, not a full JSON parser. It is suitable for the current bridge response shape only.
- The display shows at most 5 sessions.
- The default Arduino board configuration for `esp32:esp32:esp32s3` still does not fit this sketch. Use the documented `16M` / `app3M_fat9M_16MB` compile profile or the provided compile launcher.

## Next Optimization Directions

- Improve the live session pipeline.
- Keep the current exporter-to-JSON approach and enrich the exported metadata.
- Investigate whether Codex Desktop has a stable local thread store/API the bridge can read directly without an external exporter loop.
- Add paging or touch interaction for more than 5 sessions.
- Add status sorting, for example active sessions first, then blocked/waiting, then completed.
- Add per-state colors and concise Chinese labels: `active` = `工作中`, `notLoaded` = `未加载`, `complete` = `已完成`, `blocked` = `已阻塞`, fallback = `未知`.
- Add a last-seen/offline indicator when the bridge cannot be reached.
- Add a small config file for Wi-Fi/bridge URL or reduce hard-coded values in the sketch.
- Improve bridge data schema toward live use:

```json
{
  "updated_at": "2026-06-12 00:21:00",
  "sessions": [
    {
      "title": "状态屏开发",
      "state": "active",
      "status_zh": "工作中",
      "cwd": "ESP32-S3-Touch-LCD-3.49",
      "updated_at": "2026-06-12 00:21:00"
    }
  ]
}
```

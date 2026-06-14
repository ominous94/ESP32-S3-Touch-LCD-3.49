# ESP32-S3-Touch-LCD-3.49

中文wiki链接: https://www.waveshare.net/wiki/ESP32-S3-Touch-LCD-3.49<br>
Product English wiki link: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49

# Arduino example Tools configuration
![alt text](<Tools Configuration.png>)

# Codex Status build

The `Examples/Arduino/12_Codex_Status` sketch currently depends on:

- the repo-local LVGL 9 library at `Arduino_Libraries/lvgl9/lvgl`
- `esp32:esp32:esp32s3` with `FlashSize=16M`
- `PartitionScheme=app3M_fat9M_16MB`
- `CDCOnBoot=cdc` so `Serial` telemetry is visible on USB
- `PSRAM=opi` so LVGL SPIRAM frame buffers can be allocated

Use the provided launcher to compile it with the verified settings:

```cmd
compile_codex_status.cmd
```

# Codex Status end-to-end verification

Use the verification launcher to compile, upload to the auto-detected ESP32 USB serial port, capture serial output, and require `CODEX_STATUS` telemetry:

```cmd
verify_codex_status.cmd
```

To only check an already-running board without uploading new firmware:

```cmd
verify_codex_status.cmd --skip-compile --skip-upload
```

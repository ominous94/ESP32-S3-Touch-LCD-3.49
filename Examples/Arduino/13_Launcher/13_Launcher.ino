#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "i2c_bsp.h"
#include "src/button_bsp/button_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include <lvgl.h>
#include "adc_bsp.h"
#include <WiFi.h>
#include "apps/codex_status/app_codex_status.h"
#include "apps/settings/app_settings.h"
#include "src/audio_bsp/audio_bsp.h"

#define CODEX_STATUS_ENABLE_SD_TTF 1

#if LV_USE_FS_STDIO && LV_USE_TINY_TTF && LV_TINY_TTF_FILE_SUPPORT
#define CODEX_STATUS_HAS_SD_TTF_SUPPORT 1
#else
#define CODEX_STATUS_HAS_SD_TTF_SUPPORT 0
#endif

LV_FONT_DECLARE(lv_font_codex_zh_16);
LV_FONT_DECLARE(lv_font_codex_zh_20);

lv_font_t *ttf_font_16 = NULL;
lv_font_t *ttf_font_20 = NULL;

static const char *CODEX_STATUS_FONT_PATH = "S:/fonts/NotoSansSC-VF.ttf";
static const gpio_num_t SDCARD_D0_PIN = GPIO_NUM_40;
static const gpio_num_t SDCARD_CLK_PIN = GPIO_NUM_41;
static const gpio_num_t SDCARD_CMD_PIN = GPIO_NUM_39;
static const float BATTERY_VOLTAGE_MAX = 4.2f;
static const float BATTERY_VOLTAGE_MIN = 3.0f;
static const uint32_t BATTERY_POLL_INTERVAL_MS = 1000;

static uint32_t last_battery_poll_ms = 0;
static uint32_t last_wifi_name_ms = 0;
static const uint32_t WIFI_NAME_POLL_INTERVAL_MS = 1000;
static bool is_power_hold_enabled = false;
static bool is_sd_card_mounted = false;
static esp_io_expander_handle_t io_expander = NULL;
static sdmmc_card_t *sd_card = NULL;

static void init_sd_card()
{
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 4;
  mount_config.allocation_unit_size = 16 * 1024;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SDCARD_CLK_PIN;
  slot_config.cmd = SDCARD_CMD_PIN;
  slot_config.d0 = SDCARD_D0_PIN;

  esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &sd_card);
  if (err == ESP_OK && sd_card != NULL) {
    is_sd_card_mounted = true;
    Serial.println("LAUNCHER sdcard_mounted");
    sdmmc_card_print_info(stdout, sd_card);
  } else {
    is_sd_card_mounted = false;
    Serial.print("LAUNCHER sdcard_mount_failed code=");
    Serial.println((int)err);
  }
}

static void init_ttf_fonts()
{
#if CODEX_STATUS_ENABLE_SD_TTF && CODEX_STATUS_HAS_SD_TTF_SUPPORT
  lv_fs_stdio_init();

  if (!is_sd_card_mounted) {
    Serial.println("LAUNCHER ttf_fallback reason=sdcard_not_mounted");
    return;
  }

  ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, LV_FONT_KERNING_NORMAL, 8192);
  ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, LV_FONT_KERNING_NORMAL, 8192);

  if (ttf_font_16 != NULL && ttf_font_20 != NULL) {
    Serial.print("LAUNCHER ttf_font_loaded path=");
    Serial.println(CODEX_STATUS_FONT_PATH);
  } else {
    Serial.print("LAUNCHER ttf_fallback reason=create_failed path=");
    Serial.println(CODEX_STATUS_FONT_PATH);
  }
#elif CODEX_STATUS_ENABLE_SD_TTF
  Serial.println("LAUNCHER ttf_fallback reason=lvgl_ttf_disabled");
#else
  Serial.println("LAUNCHER ttf_fallback reason=sd_ttf_disabled");
#endif
}

const lv_font_t *codex_font_16()
{
  return ttf_font_16 != NULL ? ttf_font_16 : &lv_font_codex_zh_16;
}

const lv_font_t *codex_font_20()
{
  return ttf_font_20 != NULL ? ttf_font_20 : &lv_font_codex_zh_20;
}

static void tca9554_init(void)
{
  i2c_master_bus_handle_t tca9554_i2c_bus = NULL;
  ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus));
  ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 1));
  is_power_hold_enabled = true;

  // Pin 7 = ES8311 功放使能 (PA Enable)，拉高开启扬声器功放
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1));
  Serial.println("LAUNCHER pa_enabled pin7=high");
}

static void power_button_task(void *arg)
{
  (void)arg;

  for (;;) {
    EventBits_t event_bits = xEventGroupWaitBits(pwr_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    if (get_bit_button(event_bits, 1)) {
      if (is_power_hold_enabled && io_expander != NULL) {
        is_power_hold_enabled = false;
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 0);
      }
    } else if (get_bit_button(event_bits, 2)) {
      if (!is_power_hold_enabled) {
        is_power_hold_enabled = true;
      }
    }
  }
}

static int voltage_to_percent(float v)
{
  if (v <= BATTERY_VOLTAGE_MIN) return 0;
  if (v >= BATTERY_VOLTAGE_MAX) return 100;
  return (int)((v - BATTERY_VOLTAGE_MIN) / (BATTERY_VOLTAGE_MAX - BATTERY_VOLTAGE_MIN) * 100.0f + 0.5f);
}

static void poll_battery()
{
  float volts = 0;
  int raw = 0;
  adc_get_value(&volts, &raw);
  bool charging = gpio_get_level(GPIO_NUM_16) != 0;
  int pct = voltage_to_percent(volts);
  launcher_update_battery(volts, pct, charging);
}

static void poll_wifi_name()
{
  if (WiFi.getMode() & WIFI_AP) {
    // 配网模式：设备自己开着 AP，没有"连接"的 WiFi
    launcher_update_wifi_name("配网中", lv_color_hex(0xF0C86F));
  } else if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    if (ssid.length() > 12) ssid = ssid.substring(0, 12);
    String ip = WiFi.localIP().toString();
    String text = ssid + " " + ip;
    launcher_update_wifi_name(text.c_str(), lv_color_hex(0x78F0A4));
  } else {
    launcher_update_wifi_name("未连接", lv_color_hex(0xFF7E7E));
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(50);
  Serial.println("LAUNCHER boot");

  i2c_master_Init();
  tca9554_init();
  lvgl_port_init();
  init_sd_card();
  init_ttf_fonts();
  settings_load();
  adc_bsp_init();
  gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
  uint16_t bl_duty = (uint16_t)(255 - settings_get_brightness());
  lcd_bl_pwm_bsp_init(bl_duty);
  Serial.printf("LAUNCHER brightness=%d duty=%d\n", settings_get_brightness(), bl_duty);

  launcher_init();

  if (!codex_connect_wifi()) {
    Serial.println("LAUNCHER wifi_failed_entering_config");
    launcher_request_switch(app_idx::WIFI_CONFIG);
  }

  button_Init();
  xTaskCreatePinnedToCore(power_button_task, "power_button_task", 4 * 1024, NULL, 2, NULL, 1);

  // 初始化音频 codec（ES8311 DAC + 功放已在上面的 tca9554_init 中使能）
  if (audio_bsp_init() == 0) {
    audio_bsp_set_volume(settings_get_volume());
    Serial.println("LAUNCHER audio_init ok");
  } else {
    Serial.println("LAUNCHER audio_init failed (non-fatal, continuing)");
  }

  Serial.println("LAUNCHER ready");
}

void loop()
{
  if (g_boot_single_clicked) {
    g_boot_single_clicked = false;
    if (launcher_current_app() >= 0) {
      launcher_return_to_home();
    }
  }

  launcher_process_pending();

  uint32_t now_ms = millis();
  if (now_ms - last_battery_poll_ms >= BATTERY_POLL_INTERVAL_MS) {
    last_battery_poll_ms = now_ms;
    poll_battery();
  }
  if (now_ms - last_wifi_name_ms >= WIFI_NAME_POLL_INTERVAL_MS) {
    last_wifi_name_ms = now_ms;
    poll_wifi_name();
  }

  launcher_tick_current_app();

  delay(50);
}

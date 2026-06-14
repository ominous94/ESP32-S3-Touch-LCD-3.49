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

#include <HTTPClient.h>
#include <WiFi.h>
#include <lvgl.h>

#define CODEX_STATUS_ENABLE_SD_TTF 1

#if LV_USE_FS_STDIO && LV_USE_TINY_TTF && LV_TINY_TTF_FILE_SUPPORT
#define CODEX_STATUS_HAS_SD_TTF_SUPPORT 1
#else
#define CODEX_STATUS_HAS_SD_TTF_SUPPORT 0
#endif

LV_FONT_DECLARE(lv_font_codex_zh_16);
LV_FONT_DECLARE(lv_font_codex_zh_20);

extern "C" {
LV_IMAGE_DECLARE(codex_img_work);
LV_IMAGE_DECLARE(codex_img_idle);
LV_IMAGE_DECLARE(codex_img_done);
LV_IMAGE_DECLARE(codex_img_error);
}

const char *WIFI_SSID = "HiWiFi_503";
const char *WIFI_PASSWORD = "ziroom0503";
const char *STATUS_URL = "http://192.168.31.222:8787/status";

static const uint32_t STATUS_POLL_INTERVAL_MS = 3000;
static const int MAX_SESSIONS = 5;
static const int VISIBLE_SESSIONS = 3;
static const int SECONDARY_SESSIONS = 2;
static const int COMPANION_PANEL_W = 150;
static const int PRIMARY_PANEL_W = 318;
static const int SECONDARY_PANEL_W = 148;
static const int PANEL_H = 160;
static const int COMPANION_IMAGE_W = 132;
static const int COMPANION_IMAGE_H = 56;
static const int PRIMARY_TITLE_W = 286;
static const int PRIMARY_TEXT_W = 300;
static const int SECONDARY_TITLE_W = 124;
static const int SECONDARY_CARD_H = 74;
static const int PROGRESS_BAR_W = 286;
static const int DETAIL_BACK_PANEL_W = 86;
static const int DETAIL_CONTENT_W = 530;
static const char *CODEX_STATUS_FONT_PATH = "S:/fonts/NotoSansSC-VF.ttf";
static const gpio_num_t SDCARD_D0_PIN = GPIO_NUM_40;
static const gpio_num_t SDCARD_CLK_PIN = GPIO_NUM_41;
static const gpio_num_t SDCARD_CMD_PIN = GPIO_NUM_39;
static uint32_t last_status_poll_ms = 0;
static bool is_power_hold_enabled = false;
static bool is_sd_card_mounted = false;
static esp_io_expander_handle_t io_expander = NULL;
static sdmmc_card_t *sd_card = NULL;
static lv_font_t *ttf_font_16 = NULL;
static lv_font_t *ttf_font_20 = NULL;

static lv_obj_t *connection_label = NULL;
static lv_obj_t *assistant_image = NULL;
static lv_obj_t *companion_state_label = NULL;
static lv_obj_t *companion_summary_label = NULL;
static lv_obj_t *primary_status_dot = NULL;
static lv_obj_t *primary_status_label = NULL;
static lv_obj_t *primary_title_label = NULL;
static lv_obj_t *primary_meta_label = NULL;
static lv_obj_t *primary_progress_bar = NULL;
static lv_obj_t *secondary_cards[SECONDARY_SESSIONS] = {};
static lv_obj_t *secondary_dots[SECONDARY_SESSIONS] = {};
static lv_obj_t *secondary_statuses[SECONDARY_SESSIONS] = {};
static lv_obj_t *secondary_titles[SECONDARY_SESSIONS] = {};
static lv_obj_t *detail_back_button = NULL;
static lv_obj_t *detail_content_label = NULL;

enum CodexPage {
  CODEX_PAGE_HOME,
  CODEX_PAGE_DETAIL
};

static CodexPage current_page = CODEX_PAGE_HOME;
static String latest_connection;
static int selected_session_index = -1;
static int visible_session_indices[VISIBLE_SESSIONS] = {-1, -1, -1};
static bool session_press_cancelled[VISIBLE_SESSIONS] = {false, false, false};
static bool back_press_cancelled = false;

struct CodexSession {
  String title;
  String state;
  String status_zh;
  String cwd;
  String updated_at;
  String detail;
};

struct CodexStatus {
  String updated_at;
  int session_count;
  CodexSession sessions[MAX_SESSIONS];
};

static CodexStatus latest_status;

static void set_label_text(lv_obj_t *label, const String &text);

static String serial_safe_text(const String &value)
{
  String text = value;
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.replace("\"", "'");
  return text;
}

static void log_serial_field(const char *key, const String &value)
{
  Serial.print(" ");
  Serial.print(key);
  Serial.print("=\"");
  Serial.print(serial_safe_text(value));
  Serial.print("\"");
}

static void log_serial_field(const char *key, int value)
{
  Serial.print(" ");
  Serial.print(key);
  Serial.print("=");
  Serial.print(value);
}

static void log_serial_event(const char *event_line)
{
  Serial.print(event_line);
  Serial.print(" ms=");
  Serial.print(millis());
  Serial.print(" heap=");
  Serial.println(ESP.getFreeHeap());
}

static void log_status_snapshot(const char *connection, const CodexStatus &status)
{
  Serial.print("CODEX_STATUS ui_update");
  log_serial_field("connection", String(connection ? connection : ""));
  log_serial_field("sessions", status.session_count);
  log_serial_field("updated_at", status.updated_at.length() ? status.updated_at : "--");
  log_serial_field("heap", (int)ESP.getFreeHeap());
  Serial.println();

  for (int i = 0; i < status.session_count; ++i) {
    const CodexSession &session = status.sessions[i];
    Serial.print("CODEX_STATUS session");
    log_serial_field("index", i);
    log_serial_field("state", session.state);
    log_serial_field("status", session.status_zh);
    log_serial_field("title", session.title);
    log_serial_field("cwd", session.cwd);
    log_serial_field("updated_at", session.updated_at);
    Serial.println();
  }
}

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
    Serial.println("CODEX_STATUS sdcard_mounted");
    sdmmc_card_print_info(stdout, sd_card);
  } else {
    is_sd_card_mounted = false;
    Serial.print("CODEX_STATUS sdcard_mount_failed code=");
    Serial.println((int)err);
  }
}

static void init_ttf_fonts()
{
#if CODEX_STATUS_ENABLE_SD_TTF && CODEX_STATUS_HAS_SD_TTF_SUPPORT
  lv_fs_stdio_init();

  if (!is_sd_card_mounted) {
    Serial.println("CODEX_STATUS ttf_font_fallback reason=sdcard_not_mounted");
    return;
  }

  ttf_font_16 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 16, LV_FONT_KERNING_NORMAL, 8192);
  ttf_font_20 = lv_tiny_ttf_create_file_ex(CODEX_STATUS_FONT_PATH, 20, LV_FONT_KERNING_NORMAL, 8192);

  if (ttf_font_16 != NULL && ttf_font_20 != NULL) {
    Serial.print("CODEX_STATUS ttf_font_loaded path=");
    Serial.println(CODEX_STATUS_FONT_PATH);
  } else {
    Serial.print("CODEX_STATUS ttf_font_fallback reason=create_failed path=");
    Serial.println(CODEX_STATUS_FONT_PATH);
  }
#elif CODEX_STATUS_ENABLE_SD_TTF
  Serial.println("CODEX_STATUS ttf_font_fallback reason=lvgl_ttf_disabled");
#else
  Serial.println("CODEX_STATUS ttf_font_fallback reason=sd_ttf_disabled");
#endif
}

static const lv_font_t *codex_font_16()
{
  return ttf_font_16 != NULL ? ttf_font_16 : &lv_font_codex_zh_16;
}

static const lv_font_t *codex_font_20()
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

static String json_string_value(const String &json, const char *key)
{
  String needle = String("\"") + key + "\"";
  int key_pos = json.indexOf(needle);
  if (key_pos < 0) return "";

  int colon_pos = json.indexOf(':', key_pos + needle.length());
  if (colon_pos < 0) return "";

  int value_start = json.indexOf('"', colon_pos + 1);
  if (value_start < 0) return "";

  bool escaped = false;
  for (int i = value_start + 1; i < json.length(); ++i) {
    char c = json.charAt(i);
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      String value = json.substring(value_start + 1, i);
      value.replace("\\\"", "\"");
      value.replace("\\\\", "\\");
      value.replace("\\n", "\n");
      value.replace("\\r", "\r");
      return value;
    }
  }

  return "";
}

static int json_object_end(const String &json, int object_start)
{
  int depth = 0;
  bool in_string = false;
  bool escaped = false;

  for (int i = object_start; i < json.length(); ++i) {
    char c = json.charAt(i);
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }

    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) return i;
    }
  }

  return -1;
}

static CodexStatus parse_status_json(const String &json)
{
  CodexStatus status;
  status.updated_at = json_string_value(json, "updated_at");
  status.session_count = 0;

  int sessions_pos = json.indexOf("\"sessions\"");
  if (sessions_pos < 0) return status;

  int array_start = json.indexOf('[', sessions_pos);
  if (array_start < 0) return status;

  int search_pos = array_start + 1;
  while (status.session_count < MAX_SESSIONS) {
    int object_start = json.indexOf('{', search_pos);
    if (object_start < 0) break;

    int object_finish = json_object_end(json, object_start);
    if (object_finish < 0) break;

    String object_json = json.substring(object_start, object_finish + 1);
    CodexSession &session = status.sessions[status.session_count];
    session.title = json_string_value(object_json, "title");
    session.state = json_string_value(object_json, "state");
    session.status_zh = json_string_value(object_json, "status_zh");
    session.cwd = json_string_value(object_json, "cwd");
    session.updated_at = json_string_value(object_json, "updated_at");
    session.detail = json_string_value(object_json, "detail");
    status.session_count++;
    search_pos = object_finish + 1;
  }

  return status;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_color_t color, int width = PRIMARY_TEXT_W, const lv_font_t *font = NULL)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(label, color, 0);
  if (font == NULL) font = codex_font_16();
  lv_obj_set_style_text_font(label, font, 0);
  lv_label_set_text(label, text);
  return label;
}

static String status_label_text(const CodexSession &session)
{
  if (session.status_zh.length()) return session.status_zh;
  if (session.state == "active") return "工作中";
  if (session.state == "complete") return "已完成";
  if (session.state == "blocked") return "已阻塞";
  if (session.state == "notLoaded") return "未加载";
  return "未知";
}

static int state_progress_width(const String &state)
{
  if (state == "complete") return PROGRESS_BAR_W;
  if (state == "active") return 186;
  if (state == "notLoaded") return 100;
  if (state == "blocked") return 58;
  return 32;
}

static lv_color_t state_color(const String &state)
{
  if (state == "active") return lv_color_hex(0x78F0A4);
  if (state == "complete") return lv_color_hex(0x74B9FF);
  if (state == "blocked") return lv_color_hex(0xFF7E7E);
  if (state == "notLoaded") return lv_color_hex(0xF0C86F);
  return lv_color_hex(0x8FA1AD);
}

static int state_rank(const String &state)
{
  if (state == "blocked") return 0;
  if (state == "active") return 1;
  if (state == "complete") return 2;
  if (state == "notLoaded") return 3;
  return 4;
}

static const void *image_for_state(const String &state)
{
  if (state == "active") return &codex_img_work;
  if (state == "complete") return &codex_img_done;
  if (state == "blocked") return &codex_img_error;
  if (state == "notLoaded") return &codex_img_idle;
  return &codex_img_idle;
}

static bool connection_is_error(const char *connection)
{
  String text(connection ? connection : "");
  return text.indexOf("失败") >= 0 || text.indexOf("错误") >= 0 || text.indexOf("断开") >= 0;
}

static const void *image_for_status(const char *connection, const CodexStatus &status)
{
  if (connection_is_error(connection)) return &codex_img_error;

  int best_rank = 99;
  String best_state = "notLoaded";
  for (int i = 0; i < status.session_count; ++i) {
    int rank = state_rank(status.sessions[i].state);
    if (rank < best_rank) {
      best_rank = rank;
      best_state = status.sessions[i].state;
    }
  }

  return image_for_state(best_state);
}

static void build_visible_session_order(const CodexStatus &status, int order[VISIBLE_SESSIONS])
{
  bool used[MAX_SESSIONS] = {};
  for (int slot = 0; slot < VISIBLE_SESSIONS; ++slot) {
    order[slot] = -1;
    int best_index = -1;
    int best_rank = 99;

    for (int i = 0; i < status.session_count; ++i) {
      if (used[i]) continue;
      int rank = state_rank(status.sessions[i].state);
      if (rank < best_rank) {
        best_rank = rank;
        best_index = i;
      }
    }

    if (best_index < 0) return;
    used[best_index] = true;
    order[slot] = best_index;
  }
}

static String detail_text_for_session(int session_index)
{
  if (session_index < 0 || session_index >= latest_status.session_count) return "暂无会话内容";
  const String &detail = latest_status.sessions[session_index].detail;
  return detail.length() ? detail : "暂无会话内容";
}

static void create_status_ui();
static void create_detail_ui();
static void render_home_ui_locked();
static void render_detail_ui_locked();
static void bind_home_status_locked(const char *connection, const CodexStatus &status);
static void update_status_ui(const char *connection, const CodexStatus &status);

static void make_touch_event_bubble(lv_obj_t *obj)
{
  if (obj != NULL) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  }
}

static void set_card_pressed_feedback(lv_obj_t *card, bool pressed)
{
  if (card == NULL) return;
  lv_obj_set_style_bg_color(card, lv_color_hex(pressed ? 0x26313A : 0x151B20), 0);
}

static void set_secondary_card_pressed_feedback(lv_obj_t *card, bool pressed)
{
  if (card == NULL) return;
  lv_obj_set_style_bg_color(card, lv_color_hex(pressed ? 0x303C46 : 0x1D252C), 0);
}

static void set_back_button_pressed_feedback(lv_obj_t *button, bool pressed)
{
  if (button == NULL) return;
  lv_obj_set_style_bg_color(button, lv_color_hex(pressed ? 0x3A4A56 : 0x26313A), 0);
}

static bool session_pointer_inside(lv_obj_t *card)
{
  if (card == NULL) return false;

  lv_indev_t *indev = lv_indev_active();
  if (indev == NULL) return true;

  lv_point_t point;
  lv_area_t coords;
  lv_indev_get_point(indev, &point);
  lv_obj_get_coords(card, &coords);

  return point.x >= coords.x1 && point.x <= coords.x2 && point.y >= coords.y1 && point.y <= coords.y2;
}

static void session_feedback_event_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  int slot = (int)(intptr_t)lv_event_get_user_data(event);
  if (slot < 0 || slot >= VISIBLE_SESSIONS) return;

  if (code == LV_EVENT_PRESSED) {
    session_press_cancelled[slot] = false;
    if (slot == 0) set_card_pressed_feedback(target, true);
    else set_secondary_card_pressed_feedback(target, true);
  } else if (code == LV_EVENT_PRESSING) {
    if (!session_press_cancelled[slot] && !session_pointer_inside(target)) {
      session_press_cancelled[slot] = true;
      if (slot == 0) set_card_pressed_feedback(target, false);
      else set_secondary_card_pressed_feedback(target, false);
    }
  } else if (code == LV_EVENT_PRESS_LOST) {
    session_press_cancelled[slot] = true;
    if (slot == 0) set_card_pressed_feedback(target, false);
    else set_secondary_card_pressed_feedback(target, false);
  } else if (code == LV_EVENT_RELEASED) {
    if (!session_pointer_inside(target)) {
      session_press_cancelled[slot] = true;
    }
    if (slot == 0) set_card_pressed_feedback(target, false);
    else set_secondary_card_pressed_feedback(target, false);
  }
}

static void detail_back_feedback_event_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);

  if (code == LV_EVENT_PRESSED) {
    back_press_cancelled = false;
    set_back_button_pressed_feedback(target, true);
  } else if (code == LV_EVENT_PRESSING) {
    if (!back_press_cancelled && !session_pointer_inside(target)) {
      back_press_cancelled = true;
      set_back_button_pressed_feedback(target, false);
    }
  } else if (code == LV_EVENT_PRESS_LOST) {
    back_press_cancelled = true;
    set_back_button_pressed_feedback(target, false);
  } else if (code == LV_EVENT_RELEASED) {
    if (!session_pointer_inside(target)) {
      back_press_cancelled = true;
    }
    set_back_button_pressed_feedback(target, false);
  }
}

static void open_session_detail(int session_index)
{
  if (session_index < 0 || session_index >= latest_status.session_count) return;
  selected_session_index = session_index;
  current_page = CODEX_PAGE_DETAIL;
  Serial.print("CODEX_STATUS detail_open");
  log_serial_field("index", session_index);
  Serial.println();
  render_detail_ui_locked();
}

static void return_to_home()
{
  selected_session_index = -1;
  current_page = CODEX_PAGE_HOME;
  log_serial_event("CODEX_STATUS detail_back");
  render_home_ui_locked();
  bind_home_status_locked(latest_connection.c_str(), latest_status);
}

static void session_card_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  int slot = (int)(intptr_t)lv_event_get_user_data(event);
  if (slot < 0 || slot >= VISIBLE_SESSIONS) return;
  if (session_press_cancelled[slot]) return;
  if (!session_pointer_inside(target)) return;
  open_session_detail(visible_session_indices[slot]);
}

static void return_to_home_event_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  if (back_press_cancelled) return;
  if (!session_pointer_inside(target)) return;
  return_to_home();
}

static void render_home_ui_locked()
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  detail_back_button = NULL;
  detail_content_label = NULL;
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(screen, 6, 0);
  lv_obj_set_style_pad_column(screen, 6, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t *companion_panel = lv_obj_create(screen);
  lv_obj_set_size(companion_panel, COMPANION_PANEL_W, PANEL_H);
  lv_obj_clear_flag(companion_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(companion_panel, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(companion_panel, 0, 0);
  lv_obj_set_style_radius(companion_panel, 6, 0);
  lv_obj_set_style_pad_all(companion_panel, 6, 0);
  lv_obj_set_style_pad_row(companion_panel, 4, 0);
  lv_obj_set_flex_flow(companion_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(companion_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  assistant_image = lv_image_create(companion_panel);
  lv_image_set_src(assistant_image, &codex_img_idle);
  lv_image_set_scale(assistant_image, 82);
  lv_obj_set_size(assistant_image, COMPANION_IMAGE_W, COMPANION_IMAGE_H);
  lv_obj_align(assistant_image, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *companion_title = create_label(companion_panel, "Codex 伙伴", lv_color_hex(0xF7FAFC), 132, codex_font_20());
  lv_label_set_long_mode(companion_title, LV_LABEL_LONG_DOT);
  companion_state_label = create_label(companion_panel, "启动中", lv_color_hex(0x78F0A4), 132);
  lv_label_set_long_mode(companion_state_label, LV_LABEL_LONG_DOT);
  connection_label = create_label(companion_panel, "网络：启动中", lv_color_hex(0x8FB3C8), 132);
  lv_label_set_long_mode(connection_label, LV_LABEL_LONG_DOT);
  companion_summary_label = create_label(companion_panel, "会话：-- · 3秒刷新", lv_color_hex(0xC6D1D8), 132);
  lv_label_set_long_mode(companion_summary_label, LV_LABEL_LONG_DOT);

  lv_obj_t *primary_panel = lv_obj_create(screen);
  lv_obj_set_size(primary_panel, PRIMARY_PANEL_W, PANEL_H);
  lv_obj_clear_flag(primary_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(primary_panel, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(primary_panel, 0, 0);
  lv_obj_set_style_radius(primary_panel, 6, 0);
  lv_obj_set_style_pad_all(primary_panel, 8, 0);
  lv_obj_set_style_pad_row(primary_panel, 6, 0);
  lv_obj_set_flex_flow(primary_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(primary_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_flag(primary_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(primary_panel, session_feedback_event_cb, LV_EVENT_ALL, (void *)0);
  lv_obj_add_event_cb(primary_panel, session_card_event_cb, LV_EVENT_RELEASED, (void *)0);

  lv_obj_t *primary_status_row = lv_obj_create(primary_panel);
  make_touch_event_bubble(primary_status_row);
  lv_obj_set_size(primary_status_row, PRIMARY_TEXT_W, 20);
  lv_obj_clear_flag(primary_status_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(primary_status_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(primary_status_row, 0, 0);
  lv_obj_set_style_pad_all(primary_status_row, 0, 0);
  lv_obj_set_style_pad_column(primary_status_row, 6, 0);
  lv_obj_set_flex_flow(primary_status_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(primary_status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  primary_status_dot = lv_obj_create(primary_status_row);
  make_touch_event_bubble(primary_status_dot);
  lv_obj_set_size(primary_status_dot, 10, 10);
  lv_obj_set_style_radius(primary_status_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(primary_status_dot, 0, 0);
  lv_obj_set_style_bg_color(primary_status_dot, lv_color_hex(0x8FA1AD), 0);

  primary_status_label = create_label(primary_status_row, "等待状态", lv_color_hex(0x78F0A4), 92);
  make_touch_event_bubble(primary_status_label);
  lv_label_set_long_mode(primary_status_label, LV_LABEL_LONG_DOT);

  primary_title_label = create_label(primary_panel, "暂无会话", lv_color_hex(0xF7FAFC), PRIMARY_TITLE_W, codex_font_20());
  make_touch_event_bubble(primary_title_label);
  lv_obj_set_height(primary_title_label, 26);
  lv_label_set_long_mode(primary_title_label, LV_LABEL_LONG_DOT);

  primary_meta_label = create_label(primary_panel, "-- · --", lv_color_hex(0x8FA1AD), PRIMARY_TEXT_W);
  make_touch_event_bubble(primary_meta_label);
  lv_obj_set_height(primary_meta_label, 18);
  lv_label_set_long_mode(primary_meta_label, LV_LABEL_LONG_DOT);

  lv_obj_t *progress_track = lv_obj_create(primary_panel);
  make_touch_event_bubble(progress_track);
  lv_obj_set_size(progress_track, PROGRESS_BAR_W, 7);
  lv_obj_clear_flag(progress_track, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(progress_track, lv_color_hex(0x2B3640), 0);
  lv_obj_set_style_border_width(progress_track, 0, 0);
  lv_obj_set_style_radius(progress_track, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(progress_track, 0, 0);

  primary_progress_bar = lv_obj_create(progress_track);
  make_touch_event_bubble(primary_progress_bar);
  lv_obj_set_size(primary_progress_bar, 32, 7);
  lv_obj_clear_flag(primary_progress_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(primary_progress_bar, lv_color_hex(0x78F0A4), 0);
  lv_obj_set_style_border_width(primary_progress_bar, 0, 0);
  lv_obj_set_style_radius(primary_progress_bar, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_all(primary_progress_bar, 0, 0);
  lv_obj_align(primary_progress_bar, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *secondary_panel = lv_obj_create(screen);
  lv_obj_set_size(secondary_panel, SECONDARY_PANEL_W, PANEL_H);
  lv_obj_clear_flag(secondary_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(secondary_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(secondary_panel, 0, 0);
  lv_obj_set_style_pad_all(secondary_panel, 0, 0);
  lv_obj_set_style_pad_row(secondary_panel, 8, 0);
  lv_obj_set_flex_flow(secondary_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(secondary_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  for (int i = 0; i < SECONDARY_SESSIONS; ++i) {
    secondary_cards[i] = lv_obj_create(secondary_panel);
    lv_obj_set_size(secondary_cards[i], SECONDARY_PANEL_W, SECONDARY_CARD_H);
    lv_obj_clear_flag(secondary_cards[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(secondary_cards[i], lv_color_hex(0x1D252C), 0);
    lv_obj_set_style_border_width(secondary_cards[i], 0, 0);
    lv_obj_set_style_radius(secondary_cards[i], 6, 0);
    lv_obj_set_style_pad_all(secondary_cards[i], 6, 0);
    lv_obj_set_style_pad_row(secondary_cards[i], 5, 0);
    lv_obj_set_flex_flow(secondary_cards[i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(secondary_cards[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(secondary_cards[i], session_feedback_event_cb, LV_EVENT_ALL, (void *)(intptr_t)(i + 1));
    lv_obj_add_event_cb(secondary_cards[i], session_card_event_cb, LV_EVENT_RELEASED, (void *)(intptr_t)(i + 1));

    lv_obj_t *secondary_row = lv_obj_create(secondary_cards[i]);
    make_touch_event_bubble(secondary_row);
    lv_obj_set_size(secondary_row, SECONDARY_TITLE_W, 16);
    lv_obj_clear_flag(secondary_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(secondary_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(secondary_row, 0, 0);
    lv_obj_set_style_pad_all(secondary_row, 0, 0);
    lv_obj_set_style_pad_column(secondary_row, 5, 0);
    lv_obj_set_flex_flow(secondary_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(secondary_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    secondary_dots[i] = lv_obj_create(secondary_row);
    make_touch_event_bubble(secondary_dots[i]);
    lv_obj_set_size(secondary_dots[i], 8, 8);
    lv_obj_set_style_radius(secondary_dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(secondary_dots[i], 0, 0);
    lv_obj_set_style_bg_color(secondary_dots[i], lv_color_hex(0x8FA1AD), 0);

    secondary_statuses[i] = create_label(secondary_row, "--", lv_color_hex(0xC6D1D8), 100);
    make_touch_event_bubble(secondary_statuses[i]);
    lv_label_set_long_mode(secondary_statuses[i], LV_LABEL_LONG_DOT);

    secondary_titles[i] = create_label(secondary_cards[i], "--", lv_color_hex(0xE9EEF2), SECONDARY_TITLE_W);
    make_touch_event_bubble(secondary_titles[i]);
    lv_obj_set_height(secondary_titles[i], 36);
    lv_label_set_long_mode(secondary_titles[i], LV_LABEL_LONG_DOT);
    lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
  }

}

static void create_status_ui()
{
  if (!lvgl_port_lock(-1)) return;
  render_home_ui_locked();
  lvgl_port_unlock();
}

static void render_detail_ui_locked()
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(screen, 6, 0);
  lv_obj_set_style_pad_column(screen, 6, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t *back_panel = lv_obj_create(screen);
  lv_obj_set_size(back_panel, DETAIL_BACK_PANEL_W, PANEL_H);
  lv_obj_clear_flag(back_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(back_panel, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(back_panel, 0, 0);
  lv_obj_set_style_radius(back_panel, 6, 0);
  lv_obj_set_style_pad_all(back_panel, 7, 0);
  lv_obj_set_flex_flow(back_panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(back_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  detail_back_button = lv_obj_create(back_panel);
  lv_obj_set_size(detail_back_button, 68, 44);
  lv_obj_clear_flag(detail_back_button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(detail_back_button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(detail_back_button, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_border_width(detail_back_button, 0, 0);
  lv_obj_set_style_radius(detail_back_button, 6, 0);
  lv_obj_set_style_pad_all(detail_back_button, 0, 0);
  lv_obj_add_event_cb(detail_back_button, detail_back_feedback_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_event_cb(detail_back_button, return_to_home_event_cb, LV_EVENT_RELEASED, NULL);

  lv_obj_t *back_label = create_label(detail_back_button, "返回", lv_color_hex(0xF7FAFC), 68);
  make_touch_event_bubble(back_label);
  lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(back_label);

  lv_obj_t *content_panel = lv_obj_create(screen);
  lv_obj_set_size(content_panel, DETAIL_CONTENT_W, PANEL_H);
  lv_obj_set_style_bg_color(content_panel, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(content_panel, 0, 0);
  lv_obj_set_style_radius(content_panel, 6, 0);
  lv_obj_set_style_pad_all(content_panel, 8, 0);
  lv_obj_set_scroll_dir(content_panel, LV_DIR_VER);

  detail_content_label = create_label(content_panel, "", lv_color_hex(0xD8E1E7), DETAIL_CONTENT_W - 20);
  lv_label_set_long_mode(detail_content_label, LV_LABEL_LONG_WRAP);
  String detail_text = detail_text_for_session(selected_session_index);
  lv_label_set_text(detail_content_label, detail_text.c_str());
}

static void create_detail_ui()
{
  if (!lvgl_port_lock(-1)) return;
  render_detail_ui_locked();
  lvgl_port_unlock();
}

static void set_label_text(lv_obj_t *label, const String &text)
{
  if (label != NULL) {
    lv_label_set_text(label, text.c_str());
  }
}

static void bind_home_status_locked(const char *connection, const CodexStatus &status)
{
  set_label_text(connection_label, String("网络：") + connection);
  set_label_text(companion_summary_label, String("会话：") + status.session_count + "个 · 3秒刷新");

  if (assistant_image != NULL) {
    lv_image_set_src(assistant_image, image_for_status(connection, status));
  }

  int order[VISIBLE_SESSIONS] = {};
  build_visible_session_order(status, order);
  for (int i = 0; i < VISIBLE_SESSIONS; ++i) {
    visible_session_indices[i] = order[i];
  }

  if (order[0] >= 0) {
    const CodexSession &primary = status.sessions[order[0]];
    String status_text = status_label_text(primary);
    String title_text = primary.title.length() ? primary.title : "未命名会话";
    String project_text = primary.cwd.length() ? primary.cwd : "--";
    String updated_text = primary.updated_at.length() ? primary.updated_at : (status.updated_at.length() ? status.updated_at : "--");

    set_label_text(companion_state_label, status_text);
    lv_obj_set_style_text_color(companion_state_label, state_color(primary.state), 0);
    set_label_text(primary_status_label, status_text);
    lv_obj_set_style_text_color(primary_status_label, state_color(primary.state), 0);
    lv_obj_set_style_bg_color(primary_status_dot, state_color(primary.state), 0);
    set_label_text(primary_title_label, title_text);
    set_label_text(primary_meta_label, project_text + " · " + updated_text);
    lv_obj_set_width(primary_progress_bar, state_progress_width(primary.state));
    lv_obj_set_style_bg_color(primary_progress_bar, state_color(primary.state), 0);
  } else {
    String empty_state = connection_is_error(connection) ? String(connection) : String("待机");
    set_label_text(companion_state_label, empty_state);
    lv_obj_set_style_text_color(companion_state_label, connection_is_error(connection) ? lv_color_hex(0xFF7E7E) : lv_color_hex(0xF0C86F), 0);
    set_label_text(primary_status_label, empty_state);
    lv_obj_set_style_text_color(primary_status_label, lv_color_hex(0xF0C86F), 0);
    lv_obj_set_style_bg_color(primary_status_dot, lv_color_hex(0xF0C86F), 0);
    set_label_text(primary_title_label, "暂无会话");
    set_label_text(primary_meta_label, status.updated_at.length() ? status.updated_at : "--");
    lv_obj_set_width(primary_progress_bar, state_progress_width(""));
    lv_obj_set_style_bg_color(primary_progress_bar, lv_color_hex(0x8FA1AD), 0);
  }

  for (int i = 0; i < SECONDARY_SESSIONS; ++i) {
    int session_index = order[i + 1];
    if (session_index >= 0) {
      const CodexSession &session = status.sessions[session_index];
      String title_text = session.title.length() ? session.title : "未命名会话";
      String status_text = status_label_text(session);
      set_label_text(secondary_statuses[i], status_text);
      lv_obj_set_style_text_color(secondary_statuses[i], state_color(session.state), 0);
      set_label_text(secondary_titles[i], title_text);
      lv_obj_set_style_bg_color(secondary_dots[i], state_color(session.state), 0);
      lv_obj_remove_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void update_status_ui(const char *connection, const CodexStatus &status)
{
  latest_status = status;
  latest_connection = String(connection ? connection : "");

  if (!lvgl_port_lock(-1)) return;

  if (current_page == CODEX_PAGE_DETAIL) {
    if (selected_session_index < 0 || selected_session_index >= latest_status.session_count) {
      selected_session_index = -1;
      current_page = CODEX_PAGE_HOME;
      render_home_ui_locked();
      bind_home_status_locked(connection, latest_status);
      lvgl_port_unlock();
      log_status_snapshot(connection, status);
      return;
    }
    set_label_text(detail_content_label, detail_text_for_session(selected_session_index));
    lvgl_port_unlock();
    log_status_snapshot(connection, status);
    return;
  }

  bind_home_status_locked(connection, status);
  lvgl_port_unlock();
  log_status_snapshot(connection, status);
}

static void connect_wifi()
{
  CodexStatus status;
  status.session_count = 0;
  log_serial_event("CODEX_STATUS wifi_connecting");
  update_status_ui("连接中", status);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t started_ms = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started_ms < 20000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    status.updated_at = WiFi.localIP().toString();
    Serial.print("CODEX_STATUS wifi_connected");
    log_serial_field("ip", status.updated_at);
    log_serial_field("rssi", WiFi.RSSI());
    Serial.println();
    update_status_ui("已连接", status);
  } else {
    status.updated_at = "Wi-Fi 超时";
    log_serial_event("CODEX_STATUS wifi_failed");
    update_status_ui("失败", status);
  }
}

static void poll_status()
{
  CodexStatus status;

  if (WiFi.status() != WL_CONNECTED) {
    status.session_count = 0;
    status.updated_at = "Wi-Fi 断开";
    update_status_ui("已断开", status);
    WiFi.reconnect();
    return;
  }

  HTTPClient http;
  http.setTimeout(2000);
  http.begin(STATUS_URL);
  Serial.print("CODEX_STATUS http_get");
  log_serial_field("url", String(STATUS_URL));
  Serial.println();
  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String body = http.getString();
    status = parse_status_json(body);
    Serial.print("CODEX_STATUS http_ok");
    log_serial_field("code", code);
    log_serial_field("bytes", body.length());
    log_serial_field("sessions", status.session_count);
    Serial.println();
    update_status_ui("已连接", status);
  } else {
    status.session_count = 0;
    status.updated_at = String("HTTP ") + code;
    Serial.print("CODEX_STATUS http_error");
    log_serial_field("code", code);
    Serial.println();
    update_status_ui("服务错误", status);
  }

  http.end();
}

void setup()
{
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(50);
  log_serial_event("CODEX_STATUS boot");

  i2c_master_Init();
  tca9554_init();
  lvgl_port_init();
  init_sd_card();
  init_ttf_fonts();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  create_status_ui();
  button_Init();
  xTaskCreatePinnedToCore(power_button_task, "power_button_task", 4 * 1024, NULL, 2, NULL, 1);
  connect_wifi();
  poll_status();
}

void loop()
{
  uint32_t now_ms = millis();
  if (now_ms - last_status_poll_ms >= STATUS_POLL_INTERVAL_MS) {
    last_status_poll_ms = now_ms;
    poll_status();
  }

  delay(50);
}

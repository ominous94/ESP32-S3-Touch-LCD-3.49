#include "app_codex_status.h"
#include "apps/wifi_config/app_wifi_config.h"
#include "src/audio_bsp/audio_bsp.h"
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/* ========== Status Config ========== */
static const char *STATUS_URL = "http://192.168.31.222:8787/status";
/* 留空表示不鉴权。启用 bridge --token 后，应从 NVS 读取同一个 token。 */
static const char *STATUS_TOKEN = "";

/* ========== Constants ========== */
static const uint32_t STATUS_POLL_INTERVAL_MS = 3000;
static const int SESSION_DRAG_CANCEL_PX = 12;
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
static const int SECONDARY_CARD_H = 76;

/* ========== State ========== */
static lv_obj_t *g_app_scr = NULL;

static lv_obj_t *connection_label = NULL;
static lv_obj_t *assistant_image = NULL;
static lv_obj_t *companion_state_label = NULL;
static lv_obj_t *primary_status_dot = NULL;
static lv_obj_t *primary_status_label = NULL;
static lv_obj_t *primary_title_label = NULL;
static lv_obj_t *primary_meta_label = NULL;
static lv_obj_t *secondary_cards[2] = {};
static lv_obj_t *secondary_dots[2] = {};
static lv_obj_t *secondary_statuses[2] = {};
static lv_obj_t *secondary_titles[2] = {};
static lv_obj_t *secondary_metas[2] = {};

static String latest_connection;
static int visible_session_indices[3] = {-1, -1, -1};
static bool session_press_cancelled[3] = {};
static lv_point_t session_press_start[3] = {};

/* Sound toggle state */
static bool g_sound_enabled = true;
static lv_obj_t *g_sound_btn = NULL;
static const char *CODEX_SOUND_PREF_NS = "codex_sound";
static const char *CODEX_SOUND_PREF_KEY = "enabled";

struct CodexSession {
  String id;
  String title;
  String state;
  String status_zh;
  String cwd;
  String updated_at;
  String completed_at;
  String last_user_at;
};

struct CodexStatus {
  String updated_at;
  String source;
  String source_status;
  String source_error;
  int session_count;
  CodexSession sessions[5];
};

static CodexStatus latest_status;
static bool g_wifi_connected = false;
static uint32_t last_runtime_refresh_ms = 0;
static const uint32_t RUNTIME_REFRESH_INTERVAL_MS = 1000;

/* Completion flash state — highlights a card for 3s when its session
 * transitions into the "complete" state. Slot 0 = primary card, slots 1/2 =
 * the two secondary cards. */
static lv_obj_t *primary_card = NULL;
struct CardFlashState {
  lv_obj_t *card;
  bool is_primary;
  uint32_t start_ms;
  bool active;
};
static CardFlashState g_card_flash[3] = {};
static const uint32_t CARD_FLASH_DURATION_MS = 3000;
static const uint32_t CARD_FLASH_BREATH_PERIOD_MS = 1000;  // one inhale+exhale cycle
static const float CARD_FLASH_PEAK_RATIO = 0.2f;           // peak highlight blend (1/5 of full)

/* Previous snapshot of session (id, state) used to detect the
 * non-complete -> complete transition. */
static String g_prev_session_ids[5];
static String g_prev_session_states[5];
static int g_prev_session_count = 0;

/* Background poll task state */
static TaskHandle_t g_poll_task_handle = NULL;
static SemaphoreHandle_t g_poll_mutex = NULL;
static volatile bool g_poll_dirty = false;
static volatile bool g_poll_task_should_exit = false;  // 优雅退出标志
static CodexStatus g_polled_status;
static String g_polled_connection;

/* ========== Serial Helpers ========== */
static String serial_safe_text(const String &value) {
  String text = value;
  text.replace("\r", " "); text.replace("\n", " ");
  text.replace("\"", "'"); return text;
}
static void log_serial_field(const char *key, const String &value) {
  Serial.print(" "); Serial.print(key); Serial.print("=\"");
  Serial.print(serial_safe_text(value)); Serial.print("\"");
}
static void log_serial_field(const char *key, int value) {
  Serial.print(" "); Serial.print(key); Serial.print("="); Serial.print(value);
}
static void log_serial_event(const char *event_line) {
  Serial.print(event_line); Serial.print(" ms="); Serial.print(millis());
  Serial.print(" heap="); Serial.println(ESP.getFreeHeap());
}
static void log_status_snapshot(const char *connection, const CodexStatus &status) {
  Serial.print("CODEX_STATUS ui_update");
  log_serial_field("connection", String(connection ? connection : ""));
  log_serial_field("sessions", status.session_count);
  log_serial_field("source", status.source);
  log_serial_field("source_status", status.source_status);
  log_serial_field("updated_at", status.updated_at.length() ? status.updated_at : "--");
  log_serial_field("heap", (int)ESP.getFreeHeap()); Serial.println();
  for (int i = 0; i < status.session_count; ++i) {
    const CodexSession &s = status.sessions[i];
    Serial.print("CODEX_STATUS session");
    log_serial_field("index", i); log_serial_field("state", s.state);
    log_serial_field("status", s.status_zh); log_serial_field("title", s.title);
    log_serial_field("cwd", s.cwd); log_serial_field("updated_at", s.updated_at);
    log_serial_field("completed_at", s.completed_at); Serial.println();
  }
}

/* ========== JSON Parser ========== */
static String json_string_value(const String &json, const char *key) {
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
    if (escaped) { escaped = false; continue; }
    if (c == '\\') { escaped = true; continue; }
    if (c == '"') {
      String value = json.substring(value_start + 1, i);
      value.replace("\\\"", "\""); value.replace("\\\\", "\\");
      value.replace("\\n", "\n"); value.replace("\\r", "\r");
      return value;
    }
  }
  return "";
}
static int json_object_end(const String &json, int object_start) {
  int depth = 0; bool in_string = false, escaped = false;
  for (int i = object_start; i < json.length(); ++i) {
    char c = json.charAt(i);
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') in_string = true;
    else if (c == '{') depth++;
    else if (c == '}') { depth--; if (depth == 0) return i; }
  }
  return -1;
}
static CodexStatus parse_status_json(const String &json) {
  CodexStatus status;
  status.updated_at = json_string_value(json, "updated_at");
  status.source = json_string_value(json, "source");
  status.source_status = json_string_value(json, "source_status");
  status.source_error = json_string_value(json, "source_error");
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
    session.id = json_string_value(object_json, "id");
    session.title = json_string_value(object_json, "title");
    session.state = json_string_value(object_json, "state");
    session.status_zh = json_string_value(object_json, "status_zh");
    session.cwd = json_string_value(object_json, "cwd");
    session.updated_at = json_string_value(object_json, "updated_at");
    session.completed_at = json_string_value(object_json, "completed_at");
    session.last_user_at = json_string_value(object_json, "last_user_at");
    status.session_count++;
    search_pos = object_finish + 1;
  }
  return status;
}

/* ========== UI Helpers ========== */
static lv_obj_t *create_label(lv_obj_t *parent, const char *text, lv_color_t color, int width, const lv_font_t *font = NULL) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(label, color, 0);
  if (font == NULL) font = codex_font_16();
  lv_obj_set_style_text_font(label, font, 0);
  lv_label_set_text(label, text);
  return label;
}
static lv_obj_t *create_label_v2(lv_obj_t *parent, const char *text, lv_color_t color, int width) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_width(label, width > 0 ? width : LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(label, codex_font_16(), 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  return label;
}

static void set_label_text(lv_obj_t *label, const String &text) {
  if (label != NULL) lv_label_set_text(label, text.c_str());
}

static String status_label_text(const CodexSession &session) {
  if (session.status_zh.length()) return session.status_zh;
  if (session.state == "active") return "工作中";
  if (session.state == "complete") return "已完成";
  if (session.state == "blocked") return "已阻塞";
  if (session.state == "notLoaded") return "未运行";
  return "未知";
}

static int64_t parse_local_timestamp_ms(const String &value) {
  if (!value.length()) return -1;
  int y, mo, d, h, mi, s;
  if (sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return -1;
  if (y < 2000 || y > 2100) return -1;
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  time_t epoch = mktime(&t);
  if (epoch < 0) return -1;
  return (int64_t)epoch * 1000LL;
}

static String format_runtime(uint32_t elapsed_ms) {
  uint32_t total_seconds = elapsed_ms / 1000;
  uint32_t days = total_seconds / 86400;
  uint32_t hours = (total_seconds % 86400) / 3600;
  uint32_t minutes = (total_seconds % 3600) / 60;
  uint32_t seconds = total_seconds % 60;
  if (days > 0) {
    return String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
  }
  if (hours > 0) {
    return String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s";
  }
  return String(minutes) + "m " + String(seconds) + "s";
}

static String primary_meta_text(const CodexSession &primary, const CodexStatus &status) {
  String project_text = primary.cwd.length() ? primary.cwd : "--";
  String base = project_text;
  if (primary.state == "active") {
    int64_t start_ms = parse_local_timestamp_ms(primary.last_user_at);
    if (start_ms > 0) {
      struct timeval tv;
      if (gettimeofday(&tv, NULL) == 0) {
        int64_t now_ms = (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
        int64_t diff = now_ms - start_ms;
        if (diff >= 0) {
          return base + " · 运行 " + format_runtime((uint32_t)diff);
        }
      }
    }
  }
  if (primary.state == "complete" && primary.completed_at.length()) {
    return base + " · " + primary.completed_at;
  }
  String updated_text = primary.updated_at.length() ? primary.updated_at : (status.updated_at.length() ? status.updated_at : "--");
  return base + " · " + updated_text;
}

static String secondary_meta_text(const CodexSession &session, const CodexStatus &status) {
  if (session.state == "active") {
    int64_t start_ms = parse_local_timestamp_ms(session.last_user_at);
    if (start_ms > 0) {
      struct timeval tv;
      if (gettimeofday(&tv, NULL) == 0) {
        int64_t now_ms = (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
        int64_t diff = now_ms - start_ms;
        if (diff >= 0) {
          return "运行 " + format_runtime((uint32_t)diff);
        }
      }
    }
  }
  if (session.state == "complete" && session.completed_at.length()) {
    return session.completed_at;
  }
  return session.updated_at.length() ? session.updated_at : (status.updated_at.length() ? status.updated_at : "--");
}

static lv_color_t state_color(const String &state) {
  if (state == "active") return lv_color_hex(0x78F0A4);
  if (state == "complete") return lv_color_hex(0x74B9FF);
  if (state == "blocked") return lv_color_hex(0xFF7E7E);
  if (state == "notLoaded") return lv_color_hex(0xF0C86F);
  return lv_color_hex(0x8FA1AD);
}

static int state_rank(const String &state) {
  if (state == "active") return 0;
  if (state == "blocked") return 1;
  if (state == "complete") return 2;
  if (state == "notLoaded") return 3;
  return 4;
}

static const void *image_for_status(const char *connection, const CodexStatus &status) {
  String conn(connection ? connection : "");
  bool err = conn.indexOf("失败") >= 0 || conn.indexOf("错误") >= 0 ||
             conn.indexOf("断开") >= 0 || conn.indexOf("过期") >= 0 ||
             conn.indexOf("超时") >= 0;
  if (err) return &codex_img_error;
  int best_rank = 99; String best_state = "notLoaded";
  for (int i = 0; i < status.session_count; ++i) {
    int rank = state_rank(status.sessions[i].state);
    if (rank < best_rank) { best_rank = rank; best_state = status.sessions[i].state; }
  }
  if (best_state == "active") return &codex_img_work;
  if (best_state == "complete") return &codex_img_done;
  if (best_state == "blocked") return &codex_img_error;
  return &codex_img_idle;
}

static void build_visible_session_order(const CodexStatus &status, int order[3]) {
  bool used[5] = {};
  for (int slot = 0; slot < VISIBLE_SESSIONS; ++slot) {
    order[slot] = -1; int best_index = -1, best_rank = 99;
    for (int i = 0; i < status.session_count; ++i) {
      if (used[i]) continue;
      int rank = state_rank(status.sessions[i].state);
      if (rank < best_rank) { best_rank = rank; best_index = i; }
    }
    if (best_index < 0) return;
    used[best_index] = true; order[slot] = best_index;
  }
}

static void make_touch_event_bubble(lv_obj_t *obj) {
  if (obj != NULL) lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
}

/* ========== Completion Flash ========== */
static lv_color_t card_default_bg_color(bool is_primary) {
  return lv_color_hex(is_primary ? 0x151B20 : 0x1D252C);
}
static lv_color_t card_flash_highlight_color(void) {
  return lv_color_hex(0x74B9FF);  // mirrors the "complete" state color
}
static void trigger_card_flash(int slot) {
  if (slot < 0 || slot >= 3) return;
  lv_obj_t *card = (slot == 0) ? primary_card : secondary_cards[slot - 1];
  if (card == NULL) return;
  g_card_flash[slot].card = card;
  g_card_flash[slot].is_primary = (slot == 0);
  g_card_flash[slot].start_ms = millis();
  g_card_flash[slot].active = true;
  // First tick_card_flash call will set the breathing color; start from the
  // default so the card doesn't flash-bang to full highlight.
  lv_obj_set_style_bg_color(card, card_default_bg_color(g_card_flash[slot].is_primary), 0);
}
/* Drop flash state without touching styles — call before lv_obj_clean so
 * tick_card_flash never writes to a freed card. */
static void invalidate_card_flash(void) {
  for (int i = 0; i < 3; ++i) {
    g_card_flash[i].active = false;
    g_card_flash[i].card = NULL;
  }
}
/* Restore default background and clear state — call when the app is torn
 * down only if the cards are still alive; otherwise use invalidate. */
static void clear_card_flash(void) {
  for (int i = 0; i < 3; ++i) {
    if (g_card_flash[i].active && g_card_flash[i].card != NULL) {
      lv_obj_set_style_bg_color(g_card_flash[i].card, card_default_bg_color(g_card_flash[i].is_primary), 0);
    }
    g_card_flash[i].active = false;
    g_card_flash[i].card = NULL;
  }
}
static void tick_card_flash(void) {
  uint32_t now = millis();
  for (int i = 0; i < 3; ++i) {
    if (!g_card_flash[i].active) continue;
    if (g_card_flash[i].card == NULL) {
      g_card_flash[i].active = false;
      continue;
    }
    uint32_t elapsed = now - g_card_flash[i].start_ms;
    if (elapsed >= CARD_FLASH_DURATION_MS) {
      lv_obj_set_style_bg_color(g_card_flash[i].card, card_default_bg_color(g_card_flash[i].is_primary), 0);
      g_card_flash[i].active = false;
      g_card_flash[i].card = NULL;
      continue;
    }
    /* Breathing: (1 - cos(2π·phase))/2 gives a smooth 0→1→0 wave over one
     * period; scaled to PEAK_RATIO so the highlight never exceeds 1/5 blend. */
    float phase = (float)(elapsed % CARD_FLASH_BREATH_PERIOD_MS) / (float)CARD_FLASH_BREATH_PERIOD_MS;
    float wave = (1.0f - cosf(phase * 2.0f * 3.14159265f)) * 0.5f;
    uint8_t mix = (uint8_t)(wave * CARD_FLASH_PEAK_RATIO * 255.0f + 0.5f);
    lv_color_t bg = lv_color_mix(card_flash_highlight_color(),
                                 card_default_bg_color(g_card_flash[i].is_primary),
                                 mix);
    lv_obj_set_style_bg_color(g_card_flash[i].card, bg, 0);
  }
}
static bool id_in_just_completed(const String &id, const String just_completed_ids[], int just_completed_count) {
  if (!id.length()) return false;
  for (int j = 0; j < just_completed_count; ++j) {
    if (just_completed_ids[j] == id) return true;
  }
  return false;
}

/* ========== Touch Feedback ========== */
static void set_card_pressed_feedback(lv_obj_t *card, bool pressed) {
  if (card == NULL) return;
  lv_obj_set_style_bg_color(card, lv_color_hex(pressed ? 0x26313A : 0x151B20), 0);
}
static void set_secondary_card_pressed_feedback(lv_obj_t *card, bool pressed) {
  if (card == NULL) return;
  lv_obj_set_style_bg_color(card, lv_color_hex(pressed ? 0x303C46 : 0x1D252C), 0);
}
static bool session_pointer_inside(lv_obj_t *card) {
  if (card == NULL) return false;
  lv_indev_t *indev = lv_indev_active();
  if (indev == NULL) return true;
  lv_point_t point; lv_area_t coords;
  lv_indev_get_point(indev, &point); lv_obj_get_coords(card, &coords);
  return point.x >= coords.x1 && point.x <= coords.x2 && point.y >= coords.y1 && point.y <= coords.y2;
}
static bool active_pointer_point(lv_point_t *point) {
  lv_indev_t *indev = lv_indev_active();
  if (indev == NULL || point == NULL) return false;
  lv_indev_get_point(indev, point); return true;
}
static bool session_drag_exceeded(int slot) {
  if (slot < 0 || slot >= VISIBLE_SESSIONS) return true;
  lv_point_t point;
  if (!active_pointer_point(&point)) return false;
  int dx = abs(point.x - session_press_start[slot].x);
  int dy = abs(point.y - session_press_start[slot].y);
  return dx > SESSION_DRAG_CANCEL_PX || dy > SESSION_DRAG_CANCEL_PX;
}

static void session_feedback_event_cb(lv_event_t *event) {
  lv_event_code_t code = lv_event_get_code(event);
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  int slot = (int)(intptr_t)lv_event_get_user_data(event);
  if (slot < 0 || slot >= VISIBLE_SESSIONS) return;
  if (code == LV_EVENT_PRESSED) {
    session_press_cancelled[slot] = false;
    active_pointer_point(&session_press_start[slot]);
    if (slot == 0) set_card_pressed_feedback(target, true);
    else set_secondary_card_pressed_feedback(target, true);
  } else if (code == LV_EVENT_PRESSING) {
    if (!session_press_cancelled[slot] && (session_drag_exceeded(slot) || !session_pointer_inside(target))) {
      session_press_cancelled[slot] = true;
      if (slot == 0) set_card_pressed_feedback(target, false);
      else set_secondary_card_pressed_feedback(target, false);
    }
  } else if (code == LV_EVENT_PRESS_LOST) {
    session_press_cancelled[slot] = true;
    if (slot == 0) set_card_pressed_feedback(target, false);
    else set_secondary_card_pressed_feedback(target, false);
  } else if (code == LV_EVENT_RELEASED) {
    if (session_drag_exceeded(slot) || !session_pointer_inside(target)) session_press_cancelled[slot] = true;
    if (slot == 0) set_card_pressed_feedback(target, false);
    else set_secondary_card_pressed_feedback(target, false);
  }
}

/* ========== Background Poll Task ========== */
static void codex_poll_task_func(void *arg) {
  uint32_t last_poll_ms = 0;
  for (;;) {
    // 检查退出标志
    if (g_poll_task_should_exit) {
      Serial.println("CODEX_STATUS poll_task exiting");
      break;
    }

    // 如果 app 已经关闭，直接等待退出
    if (g_app_scr == NULL) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // 时间间隔检查
    uint32_t now = millis();
    if (now - last_poll_ms < STATUS_POLL_INTERVAL_MS) {
      vTaskDelay(pdMS_TO_TICKS(50));  // 短延迟，可及时响应退出
      continue;
    }
    last_poll_ms = now;

    // 再次检查退出标志
    if (g_poll_task_should_exit) break;

    /* Poll status */
    CodexStatus status;
    String connection;
    if (WiFi.status() != WL_CONNECTED) {
      g_wifi_connected = false;
      status.session_count = 0; status.updated_at = "Wi-Fi 断开";
      connection = "已断开";
      // 不在这里做 WiFi.reconnect() —— 避免阻塞
    } else {
      g_wifi_connected = true;
      HTTPClient http;
      http.setTimeout(1500);  // 大幅缩短超时
      http.begin(STATUS_URL);
      if (strlen(STATUS_TOKEN) > 0) {
        http.addHeader("Authorization", String("Bearer ") + STATUS_TOKEN);
      }
      Serial.print("CODEX_STATUS http_get"); log_serial_field("url", String(STATUS_URL)); Serial.println();
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String body = http.getString(); status = parse_status_json(body);
        Serial.print("CODEX_STATUS http_ok"); log_serial_field("code", code);
        log_serial_field("bytes", body.length()); log_serial_field("sessions", status.session_count); Serial.println();
        if (status.source_status == "stale") connection = "状态超时";
        else if (status.source_status == "error") connection = "状态错误";
        else if (status.source_status == "degraded") connection = "暂时本地";
        else connection = "已连接";
      } else {
        status.session_count = 0; status.updated_at = String("HTTP ") + code;
        Serial.print("CODEX_STATUS http_error"); log_serial_field("code", code); Serial.println();
        connection = "服务错误";
      }
      http.end();
    }

    // 再次检查退出标志
    if (g_poll_task_should_exit) break;

    /* Store results for main thread */
    if (xSemaphoreTake(g_poll_mutex, pdMS_TO_TICKS(50))) {  // 缩短锁等待时间
      g_polled_status = status;
      g_polled_connection = connection;
      g_poll_dirty = true;
      xSemaphoreGive(g_poll_mutex);
    }
  }

  // 退出前清理
  Serial.println("CODEX_STATUS poll_task exited");
  g_poll_task_handle = NULL;
  vTaskDelete(NULL);  // 删除自己
}

/* ========== Navigation ========== */
static void render_home_ui_locked();
static void bind_home_status_locked(const char *connection, const CodexStatus &status, const String just_completed_ids[], int just_completed_count);
static void update_status_ui(const char *connection, const CodexStatus &status);

/* ========== Launcher Back Button ========== */
static void app_back_cb(lv_event_t *e) {
  (void)e; launcher_request_return_home();
}

/* ========== Sound Toggle Switch ========== */
static void sound_toggle_cb(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_sound_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
  Preferences prefs;
  prefs.begin(CODEX_SOUND_PREF_NS, false);
  prefs.putBool(CODEX_SOUND_PREF_KEY, g_sound_enabled);
  prefs.end();
  Serial.print("CODEX_STATUS sound_toggle enabled=");
  Serial.println(g_sound_enabled ? 1 : 0);
}

/* ========== UI Builders ========== */
static void render_home_ui_locked() {
  invalidate_card_flash();
  primary_card = NULL;
  lv_obj_clean(g_app_scr);
  lv_obj_set_style_bg_color(g_app_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(g_app_scr, 6, 0);
  lv_obj_set_style_pad_column(g_app_scr, 6, 0);
  lv_obj_set_flex_flow(g_app_scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_app_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  /* Companion panel */
  lv_obj_t *cp = lv_obj_create(g_app_scr);
  lv_obj_set_size(cp, COMPANION_PANEL_W, PANEL_H);
  lv_obj_clear_flag(cp, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(cp, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(cp, 0, 0);
  lv_obj_set_style_radius(cp, 6, 0);
  lv_obj_set_style_pad_all(cp, 6, 0);
  lv_obj_set_style_pad_row(cp, 4, 0);
  lv_obj_set_flex_flow(cp, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(cp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  assistant_image = lv_image_create(cp);
  lv_image_set_src(assistant_image, &codex_img_idle);
  lv_image_set_scale(assistant_image, 82);
  lv_obj_set_size(assistant_image, COMPANION_IMAGE_W, COMPANION_IMAGE_H);
  lv_obj_align(assistant_image, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(assistant_image, LV_OBJ_FLAG_HIDDEN);

  companion_state_label = create_label(cp, "启动中", lv_color_hex(0x78F0A4), 132);
  lv_label_set_long_mode(companion_state_label, LV_LABEL_LONG_DOT);
  connection_label = create_label(cp, "网络：启动中", lv_color_hex(0x8FB3C8), 132);
  lv_label_set_long_mode(connection_label, LV_LABEL_LONG_DOT);

  /* Return to launcher button */
  lv_obj_t *back_btn = lv_btn_create(cp);
  lv_obj_set_size(back_btn, 100, 36);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_radius(back_btn, 6, 0);
  lv_obj_set_style_border_width(back_btn, 0, 0);
  lv_obj_add_event_cb(back_btn, app_back_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "返回主屏");
  lv_obj_center(back_label);
  lv_obj_set_style_text_font(back_label, codex_font_16(), 0);
  lv_obj_set_style_text_color(back_label, lv_color_hex(0xF7FAFC), 0);

  /* Sound toggle row — label + switch on a single flex row below the back button */
  lv_obj_t *sound_row = lv_obj_create(cp);
  lv_obj_set_size(sound_row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_clear_flag(sound_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(sound_row, lv_color_hex(0x1A2230), 0);
  lv_obj_set_style_radius(sound_row, 6, 0);
  lv_obj_set_style_border_width(sound_row, 0, 0);
  lv_obj_set_style_pad_all(sound_row, 6, 0);
  lv_obj_set_style_pad_column(sound_row, 8, 0);
  lv_obj_set_flex_flow(sound_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sound_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *sound_label = lv_label_create(sound_row);
  lv_label_set_text(sound_label, "提示音");
  lv_obj_set_style_text_font(sound_label, codex_font_16(), 0);
  lv_obj_set_style_text_color(sound_label, lv_color_hex(0xF7FAFC), 0);

  g_sound_btn = lv_switch_create(sound_row);
  lv_obj_set_size(g_sound_btn, 50, 24);
  lv_obj_add_event_cb(g_sound_btn, sound_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
  if (g_sound_enabled) {
    lv_obj_add_state(g_sound_btn, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(g_sound_btn, LV_STATE_CHECKED);
  }

  /* Primary panel */
  lv_obj_t *pp = lv_obj_create(g_app_scr);
  lv_obj_set_size(pp, PRIMARY_PANEL_W, PANEL_H);
  lv_obj_clear_flag(pp, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(pp, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(pp, 0, 0);
  lv_obj_set_style_radius(pp, 6, 0);
  lv_obj_set_style_pad_all(pp, 8, 0);
  lv_obj_set_style_pad_row(pp, 6, 0);
  lv_obj_set_flex_flow(pp, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(pp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_flag(pp, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(pp, session_feedback_event_cb, LV_EVENT_ALL, (void *)0);
  primary_card = pp;

  lv_obj_t *psr = lv_obj_create(pp);
  make_touch_event_bubble(psr);
  lv_obj_set_size(psr, PRIMARY_TEXT_W, 20); lv_obj_clear_flag(psr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(psr, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(psr, 0, 0);
  lv_obj_set_style_pad_all(psr, 0, 0); lv_obj_set_style_pad_column(psr, 6, 0);
  lv_obj_set_flex_flow(psr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(psr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  primary_status_dot = lv_obj_create(psr);
  make_touch_event_bubble(primary_status_dot);
  lv_obj_set_size(primary_status_dot, 10, 10);
  lv_obj_set_style_radius(primary_status_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(primary_status_dot, 0, 0);
  lv_obj_set_style_bg_color(primary_status_dot, lv_color_hex(0x8FA1AD), 0);

  primary_status_label = create_label(psr, "等待状态", lv_color_hex(0x78F0A4), 92);
  make_touch_event_bubble(primary_status_label);
  lv_label_set_long_mode(primary_status_label, LV_LABEL_LONG_DOT);

  primary_title_label = create_label(pp, "暂无会话", lv_color_hex(0xF7FAFC), PRIMARY_TITLE_W, codex_font_20());
  make_touch_event_bubble(primary_title_label);
  lv_obj_set_height(primary_title_label, 26);
  lv_label_set_long_mode(primary_title_label, LV_LABEL_LONG_DOT);

  primary_meta_label = create_label(pp, "-- · --", lv_color_hex(0x8FA1AD), PRIMARY_TEXT_W);
  make_touch_event_bubble(primary_meta_label);
  lv_obj_set_height(primary_meta_label, 18);
  lv_label_set_long_mode(primary_meta_label, LV_LABEL_LONG_DOT);

  /* Secondary panel */
  lv_obj_t *sp = lv_obj_create(g_app_scr);
  lv_obj_set_size(sp, SECONDARY_PANEL_W, PANEL_H);
  lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sp, 0, 0);
  lv_obj_set_style_pad_all(sp, 0, 0); lv_obj_set_style_pad_row(sp, 8, 0);
  lv_obj_set_flex_flow(sp, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  for (int i = 0; i < SECONDARY_SESSIONS; ++i) {
    secondary_cards[i] = lv_obj_create(sp);
    lv_obj_set_size(secondary_cards[i], SECONDARY_PANEL_W, SECONDARY_CARD_H);
    lv_obj_clear_flag(secondary_cards[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(secondary_cards[i], lv_color_hex(0x1D252C), 0);
    lv_obj_set_style_border_width(secondary_cards[i], 0, 0);
    lv_obj_set_style_radius(secondary_cards[i], 6, 0);
    lv_obj_set_style_pad_all(secondary_cards[i], 6, 0);
    lv_obj_set_style_pad_row(secondary_cards[i], 4, 0);
    lv_obj_set_flex_flow(secondary_cards[i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(secondary_cards[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(secondary_cards[i], session_feedback_event_cb, LV_EVENT_ALL, (void *)(intptr_t)(i + 1));

    lv_obj_t *sr = lv_obj_create(secondary_cards[i]);
    make_touch_event_bubble(sr);
    lv_obj_set_size(sr, SECONDARY_TITLE_W, 16); lv_obj_clear_flag(sr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(sr, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(sr, 0, 0);
    lv_obj_set_style_pad_all(sr, 0, 0); lv_obj_set_style_pad_column(sr, 5, 0);
    lv_obj_set_flex_flow(sr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    secondary_dots[i] = lv_obj_create(sr);
    make_touch_event_bubble(secondary_dots[i]);
    lv_obj_set_size(secondary_dots[i], 8, 8);
    lv_obj_set_style_radius(secondary_dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(secondary_dots[i], 0, 0);
    lv_obj_set_style_bg_color(secondary_dots[i], lv_color_hex(0x8FA1AD), 0);

    secondary_statuses[i] = create_label(sr, "--", lv_color_hex(0xC6D1D8), 100);
    make_touch_event_bubble(secondary_statuses[i]); lv_label_set_long_mode(secondary_statuses[i], LV_LABEL_LONG_DOT);

    secondary_titles[i] = create_label(secondary_cards[i], "--", lv_color_hex(0xE9EEF2), SECONDARY_TITLE_W);
    make_touch_event_bubble(secondary_titles[i]); lv_obj_set_height(secondary_titles[i], 20);
    lv_label_set_long_mode(secondary_titles[i], LV_LABEL_LONG_DOT);

    secondary_metas[i] = create_label(secondary_cards[i], "--", lv_color_hex(0x8FA1AD), SECONDARY_TITLE_W);
    make_touch_event_bubble(secondary_metas[i]); lv_obj_set_height(secondary_metas[i], 16);
    lv_label_set_long_mode(secondary_metas[i], LV_LABEL_LONG_DOT);

    lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
  }
}

/* ========== Status Binding ========== */
static void bind_home_status_locked(const char *connection, const CodexStatus &status, const String just_completed_ids[], int just_completed_count) {
  set_label_text(connection_label, String("网络：") + connection);

  int order[3] = {};
  build_visible_session_order(status, order);
  for (int i = 0; i < VISIBLE_SESSIONS; ++i) visible_session_indices[i] = order[i];

  if (order[0] >= 0) {
    const CodexSession &primary = status.sessions[order[0]];
    String status_text = status_label_text(primary);
    String title_text = primary.title.length() ? primary.title : "未命名会话";
    set_label_text(companion_state_label, status_text);
    lv_obj_set_style_text_color(companion_state_label, state_color(primary.state), 0);
    set_label_text(primary_status_label, status_text);
    lv_obj_set_style_text_color(primary_status_label, state_color(primary.state), 0);
    lv_obj_set_style_bg_color(primary_status_dot, state_color(primary.state), 0);
    set_label_text(primary_title_label, title_text);
    set_label_text(primary_meta_label, primary_meta_text(primary, status));
    if (id_in_just_completed(primary.id, just_completed_ids, just_completed_count)) {
      trigger_card_flash(0);
    }
  } else {
    String conn(connection ? connection : "");
    bool err = conn.indexOf("失败") >= 0 || conn.indexOf("错误") >= 0 ||
               conn.indexOf("断开") >= 0 || conn.indexOf("过期") >= 0 ||
               conn.indexOf("超时") >= 0;
    String empty_state = err ? conn : String("待机");
    lv_color_t color = err ? lv_color_hex(0xFF7E7E) : lv_color_hex(0xF0C86F);
    set_label_text(companion_state_label, empty_state);
    lv_obj_set_style_text_color(companion_state_label, color, 0);
    set_label_text(primary_status_label, empty_state);
    lv_obj_set_style_text_color(primary_status_label, lv_color_hex(0xF0C86F), 0);
    lv_obj_set_style_bg_color(primary_status_dot, lv_color_hex(0xF0C86F), 0);
    set_label_text(primary_title_label, "暂无会话");
    set_label_text(primary_meta_label, status.updated_at.length() ? status.updated_at : "--");
  }

  for (int i = 0; i < SECONDARY_SESSIONS; ++i) {
    int session_index = order[i + 1];
    if (session_index >= 0) {
      const CodexSession &session = status.sessions[session_index];
      set_label_text(secondary_statuses[i], status_label_text(session));
      lv_obj_set_style_text_color(secondary_statuses[i], state_color(session.state), 0);
      set_label_text(secondary_titles[i], session.title.length() ? session.title : "未命名会话");
      set_label_text(secondary_metas[i], secondary_meta_text(session, status));
      lv_obj_set_style_bg_color(secondary_dots[i], state_color(session.state), 0);
      lv_obj_remove_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
      if (id_in_just_completed(session.id, just_completed_ids, just_completed_count)) {
        trigger_card_flash(i + 1);
      }
    } else {
      lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void update_status_ui(const char *connection, const CodexStatus &status) {
  /* Detect sessions that just transitioned into "complete" by comparing
   * against the previous snapshot. Only sessions already seen before are
   * eligible — newly-appeared sessions are not a transition. This reads
   * g_prev_* (main-thread-only) so it is safe outside the LVGL lock. */
  String just_completed_ids[5];
  int just_completed_count = 0;
  if (g_prev_session_count > 0) {
    for (int i = 0; i < status.session_count; ++i) {
      if (status.sessions[i].state != "complete") continue;
      const String &id = status.sessions[i].id;
      if (!id.length()) continue;
      for (int j = 0; j < g_prev_session_count; ++j) {
        if (g_prev_session_ids[j] == id) {
          if (g_prev_session_states[j] != "complete") {
            just_completed_ids[just_completed_count++] = id;
          }
          break;
        }
      }
    }
  }

  if (!lvgl_port_lock(200)) return;  // 缩短超时
  /* Mutate the global snapshot under the LVGL lock. */
  latest_status = status;
  latest_connection = String(connection ? connection : "");
  for (int i = 0; i < status.session_count; ++i) {
    g_prev_session_ids[i] = status.sessions[i].id;
    g_prev_session_states[i] = status.sessions[i].state;
  }
  g_prev_session_count = status.session_count;

  bind_home_status_locked(connection, status, just_completed_ids, just_completed_count);
  lvgl_port_unlock();

  // 任务完成提示音：有任意会话从非完成变为完成时播放（受开关控制）
  if (just_completed_count > 0 && g_sound_enabled) {
    audio_bsp_play_complete_sound();
    Serial.print("CODEX_STATUS complete_sound_triggered count=");
    Serial.println(just_completed_count);
  }

  log_status_snapshot(connection, status);
}

/* ========== WiFi & HTTP Polling ========== */
bool codex_wifi_is_connected() { return g_wifi_connected; }

bool codex_connect_wifi() {
  // NVS 无凭据时直接返回 false，让开机自动进入配网模式。
  char nvs_ssid[33] = {0};
  char nvs_pwd[64]  = {0};
  if (!wifi_config_get_stored(nvs_ssid, sizeof(nvs_ssid), nvs_pwd, sizeof(nvs_pwd))) {
    Serial.println("CODEX_STATUS wifi_no_stored_entering_config");
    g_wifi_connected = false;
    return false;
  }
  Serial.print("CODEX_STATUS wifi_using_nvs ssid=");
  Serial.println(nvs_ssid);
  log_serial_event("CODEX_STATUS wifi_connecting");
  WiFi.mode(WIFI_STA); WiFi.begin(nvs_ssid, nvs_pwd);
  uint32_t started_ms = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started_ms < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    g_wifi_connected = true;
    String ip = WiFi.localIP().toString();
    Serial.print("CODEX_STATUS wifi_connected"); log_serial_field("ip", ip);
    log_serial_field("rssi", WiFi.RSSI()); Serial.println();
    configTzTime("CST-8", "pool.ntp.org", "ntp.aliyun.com");
    Serial.println("CODEX_STATUS ntp_started");
    return true;
  } else {
    g_wifi_connected = false;
    log_serial_event("CODEX_STATUS wifi_failed");
    return false;
  }
}

/* ========== App Lifecycle ========== */
lv_obj_t *app_codex_create(void) {
  /* Load sound toggle preference from NVS */
  {
    Preferences prefs;
    prefs.begin(CODEX_SOUND_PREF_NS, true);
    g_sound_enabled = prefs.getBool(CODEX_SOUND_PREF_KEY, true);
    prefs.end();
  }

  g_app_scr = lv_obj_create(NULL);
  lv_obj_clear_flag(g_app_scr, LV_OBJ_FLAG_SCROLLABLE);
  render_home_ui_locked();

  g_poll_mutex = xSemaphoreCreateMutex();
  g_poll_dirty = false;
  g_poll_task_should_exit = false;
  if (g_poll_mutex == NULL) {
    Serial.println("CODEX_STATUS poll_mutex_create_failed");
    return g_app_scr;
  }
  BaseType_t task_result = xTaskCreatePinnedToCore(
      codex_poll_task_func, "codex_poll", 8192, NULL, 1, &g_poll_task_handle, 0);
  if (task_result != pdPASS) {
    Serial.println("CODEX_STATUS poll_task_create_failed");
    vSemaphoreDelete(g_poll_mutex);
    g_poll_mutex = NULL;
    g_poll_task_handle = NULL;
  }

  return g_app_scr;
}

void app_codex_destroy(lv_obj_t *scr) {
  (void)scr;

  // 1. 先设置 g_app_scr 为 NULL，告诉后台任务不要继续处理
  g_app_scr = NULL;

  // 2. 通知后台任务优雅退出
  if (g_poll_task_handle != NULL) {
    g_poll_task_should_exit = true;

    // 3. 等待时间覆盖 1500ms HTTP 超时，避免在网络栈调用中强制删除任务。
    int wait_count = 0;
    while (g_poll_task_handle != NULL && wait_count < 180) {
      vTaskDelay(pdMS_TO_TICKS(10));
      wait_count++;
    }

    // 4. 如果任务还没退出，才强制删除
    if (g_poll_task_handle != NULL) {
      Serial.println("CODEX_STATUS poll_task force delete");
      vTaskDelete(g_poll_task_handle);
      g_poll_task_handle = NULL;
    }
  }

  // 5. 删除互斥锁
  if (g_poll_mutex != NULL) {
    vSemaphoreDelete(g_poll_mutex);
    g_poll_mutex = NULL;
  }

  // 6. 清空所有 widget 指针
  invalidate_card_flash();
  primary_card = NULL;
  connection_label = NULL; assistant_image = NULL; companion_state_label = NULL;
  primary_status_dot = NULL; primary_status_label = NULL;
  primary_title_label = NULL; primary_meta_label = NULL;
  for (int i = 0; i < SECONDARY_SESSIONS; i++) {
    secondary_cards[i] = NULL; secondary_dots[i] = NULL;
    secondary_statuses[i] = NULL; secondary_titles[i] = NULL;
    secondary_metas[i] = NULL;
  }
  g_poll_dirty = false;
  g_poll_task_should_exit = false;
  g_prev_session_count = 0;
  g_sound_btn = NULL;
}

void app_codex_tick(lv_obj_t *scr) {
  (void)scr;
  if (g_poll_dirty && g_poll_mutex != NULL && xSemaphoreTake(g_poll_mutex, pdMS_TO_TICKS(50))) {
    g_poll_dirty = false;
    CodexStatus status = g_polled_status;
    String connection = g_polled_connection;
    xSemaphoreGive(g_poll_mutex);
    update_status_ui(connection.c_str(), status);
  }
  if (g_app_scr != NULL && lvgl_port_lock(50)) {
    tick_card_flash();
    lvgl_port_unlock();
  }
  uint32_t now_ms = millis();
  if (g_app_scr != NULL &&
      now_ms - last_runtime_refresh_ms >= RUNTIME_REFRESH_INTERVAL_MS) {
    last_runtime_refresh_ms = now_ms;
    if (lvgl_port_lock(50)) {
      if (visible_session_indices[0] >= 0 &&
          visible_session_indices[0] < latest_status.session_count) {
        const CodexSession &primary = latest_status.sessions[visible_session_indices[0]];
        if (primary.state == "active") {
          set_label_text(primary_meta_label, primary_meta_text(primary, latest_status));
        }
      }
      for (int i = 0; i < SECONDARY_SESSIONS; ++i) {
        int idx = visible_session_indices[i + 1];
        if (idx >= 0 && idx < latest_status.session_count) {
          const CodexSession &session = latest_status.sessions[idx];
          if (session.state == "active") {
            set_label_text(secondary_metas[i], secondary_meta_text(session, latest_status));
          }
        }
      }
      lvgl_port_unlock();
    }
  }
}

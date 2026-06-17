#include "app_codex_status.h"
#include "adc_bsp.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/* ========== WiFi & Status Config ========== */
static const char *WIFI_SSID = "HiWiFi_503";
static const char *WIFI_PASSWORD = "ziroom0503";
static const char *STATUS_URL = "http://192.168.31.222:8787/status";

/* ========== Constants ========== */
static const uint32_t STATUS_POLL_INTERVAL_MS = 3000;
static const uint32_t BATTERY_POLL_INTERVAL_MS = 1000;
static const float BATTERY_VOLTAGE_MAX = 4.2f;
static const float BATTERY_VOLTAGE_MIN = 3.0f;
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
static const int DETAIL_BACK_PANEL_W = 86;
static const int DETAIL_CONTENT_W = 530;

/* ========== State ========== */
static lv_obj_t *g_app_scr = NULL;
static uint32_t last_battery_poll_ms = 0;

static lv_obj_t *connection_label = NULL;
static lv_obj_t *assistant_image = NULL;
static lv_obj_t *battery_pct_label = NULL;
static lv_obj_t *battery_fill = NULL;
static lv_obj_t *battery_body = NULL;
static lv_obj_t *battery_terminal = NULL;
static lv_obj_t *bolt_line = NULL;
static lv_obj_t *companion_state_label = NULL;
static lv_obj_t *primary_status_dot = NULL;
static lv_obj_t *primary_status_label = NULL;
static lv_obj_t *primary_title_label = NULL;
static lv_obj_t *primary_meta_label = NULL;
static lv_obj_t *secondary_cards[2] = {};
static lv_obj_t *secondary_dots[2] = {};
static lv_obj_t *secondary_statuses[2] = {};
static lv_obj_t *secondary_titles[2] = {};
static lv_obj_t *detail_back_button = NULL;
static lv_obj_t *detail_content_label = NULL;

enum CodexPage { CODEX_PAGE_HOME, CODEX_PAGE_DETAIL };
static CodexPage current_page = CODEX_PAGE_HOME;
static String latest_connection;
static int selected_session_index = -1;
static int visible_session_indices[3] = {-1, -1, -1};
static bool session_press_cancelled[3] = {};
static lv_point_t session_press_start[3] = {};
static bool back_press_cancelled = false;

struct CodexSession {
  String id;
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
  CodexSession sessions[5];
};

static CodexStatus latest_status;
static bool g_wifi_connected = false;

/* Background poll task state */
static TaskHandle_t g_poll_task_handle = NULL;
static SemaphoreHandle_t g_poll_mutex = NULL;
static volatile bool g_poll_dirty = false;
static volatile bool g_poll_task_should_exit = false;  // 优雅退出标志
static CodexStatus g_polled_status;
static String g_polled_connection;
static char g_viewed_id_buf[64] = {};
static volatile bool g_viewed_pending = false;

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
  log_serial_field("updated_at", status.updated_at.length() ? status.updated_at : "--");
  log_serial_field("heap", (int)ESP.getFreeHeap()); Serial.println();
  for (int i = 0; i < status.session_count; ++i) {
    const CodexSession &s = status.sessions[i];
    Serial.print("CODEX_STATUS session");
    log_serial_field("index", i); log_serial_field("state", s.state);
    log_serial_field("status", s.status_zh); log_serial_field("title", s.title);
    log_serial_field("cwd", s.cwd); log_serial_field("updated_at", s.updated_at);
    Serial.println();
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
    session.detail = json_string_value(object_json, "detail");
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
  bool err = conn.indexOf("失败") >= 0 || conn.indexOf("错误") >= 0 || conn.indexOf("断开") >= 0;
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

static String detail_text_for_session(int session_index) {
  if (session_index < 0 || session_index >= latest_status.session_count) return "暂无会话内容";
  const String &detail = latest_status.sessions[session_index].detail;
  return detail.length() ? detail : "暂无会话内容";
}

static void make_touch_event_bubble(lv_obj_t *obj) {
  if (obj != NULL) lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
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
static void set_back_button_pressed_feedback(lv_obj_t *button, bool pressed) {
  if (button == NULL) return;
  lv_obj_set_style_bg_color(button, lv_color_hex(pressed ? 0x3A4A56 : 0x26313A), 0);
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

static void detail_back_feedback_event_cb(lv_event_t *event) {
  lv_event_code_t code = lv_event_get_code(event);
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  if (code == LV_EVENT_PRESSED) {
    back_press_cancelled = false; set_back_button_pressed_feedback(target, true);
  } else if (code == LV_EVENT_PRESSING) {
    if (!back_press_cancelled && !session_pointer_inside(target)) {
      back_press_cancelled = true; set_back_button_pressed_feedback(target, false);
    }
  } else if (code == LV_EVENT_PRESS_LOST) {
    back_press_cancelled = true; set_back_button_pressed_feedback(target, false);
  } else if (code == LV_EVENT_RELEASED) {
    if (!session_pointer_inside(target)) back_press_cancelled = true;
    set_back_button_pressed_feedback(target, false);
  }
}

/* ========== HTTP Viewed ========== */
static String viewed_url_for_session(const String &session_id) {
  String base_url = String(STATUS_URL);
  int status_pos = base_url.lastIndexOf("/status");
  if (status_pos >= 0) base_url = base_url.substring(0, status_pos);
  return base_url + "/viewed?id=" + session_id;
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

    /* Flush pending viewed notification */
    if (g_viewed_pending) {
      g_viewed_pending = false;
      String vid = String(g_viewed_id_buf);
      if (vid.length() && WiFi.status() == WL_CONNECTED) {
        String url = viewed_url_for_session(vid);
        HTTPClient http;
        http.setTimeout(1000);  // 缩短超时
        http.begin(url);
        int code = http.GET();
        Serial.print("CODEX_STATUS viewed"); log_serial_field("id", vid);
        log_serial_field("code", code); Serial.println();
        http.end();
      }
    }

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
      Serial.print("CODEX_STATUS http_get"); log_serial_field("url", String(STATUS_URL)); Serial.println();
      int code = http.GET();
      if (code == HTTP_CODE_OK) {
        String body = http.getString(); status = parse_status_json(body);
        Serial.print("CODEX_STATUS http_ok"); log_serial_field("code", code);
        log_serial_field("bytes", body.length()); log_serial_field("sessions", status.session_count); Serial.println();
        connection = "已连接";
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
static void render_detail_ui_locked();
static void bind_home_status_locked(const char *connection, const CodexStatus &status);
static void update_status_ui(const char *connection, const CodexStatus &status);

static void open_session_detail(int session_index) {
  if (session_index < 0 || session_index >= latest_status.session_count) return;
  selected_session_index = session_index;
  current_page = CODEX_PAGE_DETAIL;
  const CodexSession &s = latest_status.sessions[session_index];
  if (s.state == "complete" && s.id.length()) {
    strncpy(g_viewed_id_buf, s.id.c_str(), sizeof(g_viewed_id_buf) - 1);
    g_viewed_id_buf[sizeof(g_viewed_id_buf) - 1] = '\0';
    g_viewed_pending = true;
  }
  Serial.print("CODEX_STATUS detail_open"); log_serial_field("index", session_index); Serial.println();
  render_detail_ui_locked();
}

static void return_to_home() {
  selected_session_index = -1;
  current_page = CODEX_PAGE_HOME;
  g_viewed_pending = false;
  log_serial_event("CODEX_STATUS detail_back");
  render_home_ui_locked();
  bind_home_status_locked(latest_connection.c_str(), latest_status);
}

static void session_card_event_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  int slot = (int)(intptr_t)lv_event_get_user_data(event);
  if (slot < 0 || slot >= VISIBLE_SESSIONS) return;
  if (session_press_cancelled[slot]) return;
  if (!session_pointer_inside(target)) return;
  open_session_detail(visible_session_indices[slot]);
}

static void return_to_home_event_cb(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
  lv_obj_t *target = (lv_obj_t *)lv_event_get_current_target(event);
  if (back_press_cancelled) return;
  if (!session_pointer_inside(target)) return;
  return_to_home();
}

/* ========== Launcher Back Button ========== */
static void app_back_cb(lv_event_t *e) {
  (void)e; launcher_request_return_home();
}

/* ========== UI Builders ========== */
static void render_home_ui_locked() {
  lv_obj_clean(g_app_scr);
  detail_back_button = NULL; detail_content_label = NULL;
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

  /* Battery in companion */
  lv_obj_t *battery_row = lv_obj_create(cp);
  lv_obj_set_size(battery_row, 132, 24);
  lv_obj_clear_flag(battery_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(battery_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(battery_row, 0, 0);
  lv_obj_set_style_pad_all(battery_row, 0, 0);
  lv_obj_set_style_pad_column(battery_row, 4, 0);
  lv_obj_set_flex_flow(battery_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(battery_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *battery_icon = lv_obj_create(battery_row);
  lv_obj_set_size(battery_icon, 32, 16);
  lv_obj_clear_flag(battery_icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(battery_icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(battery_icon, 0, 0);
  lv_obj_set_style_pad_all(battery_icon, 0, 0);

  battery_body = lv_obj_create(battery_icon);
  lv_obj_set_size(battery_body, 28, 14);
  lv_obj_set_pos(battery_body, 0, 1);
  lv_obj_set_style_bg_opa(battery_body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(battery_body, 1, 0);
  lv_obj_set_style_border_color(battery_body, lv_color_hex(0xF7FAFC), 0);
  lv_obj_set_style_radius(battery_body, 3, 0);
  lv_obj_set_style_pad_all(battery_body, 2, 0);

  battery_fill = lv_obj_create(battery_body);
  lv_obj_set_size(battery_fill, 0, 10);
  lv_obj_set_pos(battery_fill, 0, 0);
  lv_obj_set_style_bg_color(battery_fill, lv_color_hex(0x78F0A4), 0);
  lv_obj_set_style_border_width(battery_fill, 0, 0);
  lv_obj_set_style_radius(battery_fill, 1, 0);

  battery_terminal = lv_obj_create(battery_icon);
  lv_obj_set_size(battery_terminal, 3, 6);
  lv_obj_set_pos(battery_terminal, 28, 5);
  lv_obj_set_style_bg_color(battery_terminal, lv_color_hex(0xF7FAFC), 0);
  lv_obj_set_style_border_width(battery_terminal, 0, 0);
  lv_obj_set_style_radius(battery_terminal, 1, 0);

  bolt_line = lv_line_create(battery_icon);
  static lv_point_precise_t bolt_pts[] = {{9, 4}, {5, 8}, {10, 8}, {7, 12}};
  lv_line_set_points(bolt_line, bolt_pts, 4);
  lv_obj_set_style_line_width(bolt_line, 2, 0);
  lv_obj_set_style_line_color(bolt_line, lv_color_hex(0xF0C86F), 0);
  lv_obj_set_style_line_rounded(bolt_line, true, 0);
  lv_obj_add_flag(bolt_line, LV_OBJ_FLAG_HIDDEN);

  battery_pct_label = create_label(battery_row, "--%", lv_color_hex(0xF7FAFC), 56, codex_font_20());

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
  lv_obj_add_event_cb(pp, session_card_event_cb, LV_EVENT_RELEASED, (void *)0);

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
    lv_obj_set_style_pad_row(secondary_cards[i], 5, 0);
    lv_obj_set_flex_flow(secondary_cards[i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(secondary_cards[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(secondary_cards[i], session_feedback_event_cb, LV_EVENT_ALL, (void *)(intptr_t)(i + 1));
    lv_obj_add_event_cb(secondary_cards[i], session_card_event_cb, LV_EVENT_RELEASED, (void *)(intptr_t)(i + 1));

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
    lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
  }
}

static void render_detail_ui_locked() {
  lv_obj_clean(g_app_scr);
  // Clear home widget pointers - they're deleted by lv_obj_clean
  connection_label = NULL;
  assistant_image = NULL;
  battery_pct_label = NULL;
  battery_fill = NULL;
  battery_body = NULL;
  battery_terminal = NULL;
  bolt_line = NULL;
  companion_state_label = NULL;
  primary_status_dot = NULL;
  primary_status_label = NULL;
  primary_title_label = NULL;
  primary_meta_label = NULL;
  for (int i = 0; i < SECONDARY_SESSIONS; i++) {
    secondary_cards[i] = NULL;
    secondary_dots[i] = NULL;
    secondary_statuses[i] = NULL;
    secondary_titles[i] = NULL;
  }
  lv_obj_set_style_bg_color(g_app_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(g_app_scr, 6, 0);
  lv_obj_set_style_pad_column(g_app_scr, 6, 0);
  lv_obj_set_flex_flow(g_app_scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_app_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t *content_panel = lv_obj_create(g_app_scr);
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

  lv_obj_t *back_panel = lv_obj_create(g_app_scr);
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

  lv_obj_t *back_lbl = create_label(detail_back_button, "返回", lv_color_hex(0xF7FAFC), 68);
  make_touch_event_bubble(back_lbl);
  lv_obj_set_style_text_align(back_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(back_lbl);
}

/* ========== Status Binding ========== */
static void bind_home_status_locked(const char *connection, const CodexStatus &status) {
  set_label_text(connection_label, String("网络：") + connection);

  int order[3] = {};
  build_visible_session_order(status, order);
  for (int i = 0; i < VISIBLE_SESSIONS; ++i) visible_session_indices[i] = order[i];

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
  } else {
    String conn(connection ? connection : "");
    bool err = conn.indexOf("失败") >= 0 || conn.indexOf("错误") >= 0 || conn.indexOf("断开") >= 0;
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
      lv_obj_set_style_bg_color(secondary_dots[i], state_color(session.state), 0);
      lv_obj_remove_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(secondary_cards[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void update_status_ui(const char *connection, const CodexStatus &status) {
  latest_status = status;
  latest_connection = String(connection ? connection : "");
  if (!lvgl_port_lock(200)) return;  // 缩短超时
  if (current_page == CODEX_PAGE_DETAIL) {
    if (selected_session_index < 0 || selected_session_index >= latest_status.session_count) {
      selected_session_index = -1; current_page = CODEX_PAGE_HOME;
      render_home_ui_locked(); bind_home_status_locked(connection, latest_status);
      lvgl_port_unlock(); log_status_snapshot(connection, status); return;
    }
    set_label_text(detail_content_label, detail_text_for_session(selected_session_index));
    lvgl_port_unlock(); log_status_snapshot(connection, status); return;
  }
  bind_home_status_locked(connection, status);
  lvgl_port_unlock();
  log_status_snapshot(connection, status);
}

/* ========== WiFi & HTTP Polling ========== */
bool codex_wifi_is_connected() { return g_wifi_connected; }

void codex_connect_wifi() {
  log_serial_event("CODEX_STATUS wifi_connecting");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t started_ms = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started_ms < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    g_wifi_connected = true;
    String ip = WiFi.localIP().toString();
    Serial.print("CODEX_STATUS wifi_connected"); log_serial_field("ip", ip);
    log_serial_field("rssi", WiFi.RSSI()); Serial.println();
  } else {
    g_wifi_connected = false;
    log_serial_event("CODEX_STATUS wifi_failed");
  }
}

/* ========== Battery Polling (Codex internal) ========== */
static int voltage_to_percent(float v) {
  if (v <= BATTERY_VOLTAGE_MIN) return 0;
  if (v >= BATTERY_VOLTAGE_MAX) return 100;
  return (int)((v - BATTERY_VOLTAGE_MIN) / (BATTERY_VOLTAGE_MAX - BATTERY_VOLTAGE_MIN) * 100.0f + 0.5f);
}
static lv_color_t percent_color(int pct) {
  if (pct < 20) return lv_color_hex(0xFF7E7E);
  if (pct < 60) return lv_color_hex(0xF0C86F);
  return lv_color_hex(0x78F0A4);
}
static void poll_battery() {
  // Don't poll battery UI when on detail page (no battery widgets there)
  if (current_page == CODEX_PAGE_DETAIL) return;
  if (battery_pct_label == NULL || battery_fill == NULL || battery_body == NULL) return;
  float volts = 0; int raw = 0;
  adc_get_value(&volts, &raw);
  bool charging = gpio_get_level(GPIO_NUM_16) != 0;
  int pct = voltage_to_percent(volts);
  lv_color_t color = percent_color(pct);
  int fill_w = pct * 24 / 100;
  if (fill_w < 0) fill_w = 0; if (fill_w > 24) fill_w = 24;
  if (lvgl_port_lock(-1)) {
    char buf[16]; snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(battery_pct_label, buf);
    lv_obj_set_style_text_color(battery_pct_label, color, 0);
    lv_obj_set_width(battery_fill, fill_w);
    lv_obj_set_style_bg_color(battery_fill, color, 0);
    lv_obj_set_style_border_color(battery_body, color, 0);
    if (battery_terminal != NULL) {
      lv_obj_set_style_bg_color(battery_terminal, color, 0);
    }
    if (bolt_line != NULL) {
      if (charging) lv_obj_clear_flag(bolt_line, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(bolt_line, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
  }
}

/* ========== App Lifecycle ========== */
lv_obj_t *app_codex_create(void) {
  g_app_scr = lv_obj_create(NULL);
  lv_obj_clear_flag(g_app_scr, LV_OBJ_FLAG_SCROLLABLE);
  render_home_ui_locked();
  last_battery_poll_ms = millis();

  g_poll_mutex = xSemaphoreCreateMutex();
  g_poll_dirty = false;
  g_viewed_pending = false;
  g_poll_task_should_exit = false;  // 清除退出标志
  xTaskCreatePinnedToCore(codex_poll_task_func, "codex_poll", 8192, NULL, 1, &g_poll_task_handle, 0);

  return g_app_scr;
}

void app_codex_destroy(lv_obj_t *scr) {
  (void)scr;

  // 1. 先设置 g_app_scr 为 NULL，告诉后台任务不要继续处理
  g_app_scr = NULL;

  // 2. 通知后台任务优雅退出
  if (g_poll_task_handle != NULL) {
    g_poll_task_should_exit = true;

    // 3. 等待任务退出（最多 200ms）
    int wait_count = 0;
    while (g_poll_task_handle != NULL && wait_count < 20) {
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
  connection_label = NULL; assistant_image = NULL;
  battery_pct_label = NULL; battery_fill = NULL; battery_body = NULL;
  battery_terminal = NULL; bolt_line = NULL; companion_state_label = NULL;
  primary_status_dot = NULL; primary_status_label = NULL;
  primary_title_label = NULL; primary_meta_label = NULL;
  for (int i = 0; i < SECONDARY_SESSIONS; i++) {
    secondary_cards[i] = NULL; secondary_dots[i] = NULL;
    secondary_statuses[i] = NULL; secondary_titles[i] = NULL;
  }
  detail_back_button = NULL; detail_content_label = NULL;
  current_page = CODEX_PAGE_HOME;
  selected_session_index = -1;
  g_poll_dirty = false; g_viewed_pending = false;
  g_poll_task_should_exit = false;
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
  uint32_t now_ms = millis();
  if (now_ms - last_battery_poll_ms >= BATTERY_POLL_INTERVAL_MS) {
    last_battery_poll_ms = now_ms;
    poll_battery();
  }
}

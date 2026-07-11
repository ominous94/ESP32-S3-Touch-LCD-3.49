#include "app_wifi_config.h"
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>

/* ========== 常量 ========== */
static const char *kWifiPrefNs = "launcher";
static const char *kKeyWifiSsid    = "wifi_ssid";
static const char *kKeyWifiPwd     = "wifi_pwd";
static const char *kApSsidPrefix   = "zhy-Launcher-";
static const uint32_t kReconnectDelayMs = 800;   // 提交后等 STA 切换
static const uint32_t kReturnHomeMs = 4000;      // 连上后多久返回主页

/* ========== 全局状态 ========== */
static lv_obj_t *g_wifi_scr            = NULL;
static lv_obj_t *g_ap_ssid_label  = NULL;
static lv_obj_t *g_status_label   = NULL;
static lv_obj_t *g_back_btn       = NULL;

static WebServer *g_server        = NULL;
static bool       g_ap_running    = false;
static String     g_ap_ssid;

// 配网提交后的状态机
enum class ConnectPhase {
  Idle,            // 等待用户提交
  Submitted,       // 收到 /save，准备切回 STA
  Connecting,      // 已 begin()，等连接
  Connected,       // 已连上，倒计时返回主页
  Failed,          // 连接超时，提示失败
};
static ConnectPhase g_phase = ConnectPhase::Idle;
static uint32_t     g_phase_ts = 0;
static String       g_new_ssid;
static String       g_new_pwd;

/* ========== NVS 存取 ========== */
bool wifi_config_has_stored(void) {
  Preferences p;
  p.begin(kWifiPrefNs, true);
  bool has = p.isKey(kKeyWifiSsid);
  p.end();
  return has;
}

bool wifi_config_get_stored(char *out_ssid, size_t ssid_cap,
                            char *out_password, size_t pwd_cap) {
  if (out_ssid == NULL || ssid_cap == 0) return false;
  Preferences p;
  p.begin(kWifiPrefNs, true);
  bool has = p.isKey(kKeyWifiSsid);
  if (has) {
    String s = p.getString(kKeyWifiSsid, "");
    String pw = p.getString(kKeyWifiPwd, "");
    strncpy(out_ssid, s.c_str(), ssid_cap - 1);
    out_ssid[ssid_cap - 1] = '\0';
    if (out_password != NULL && pwd_cap > 0) {
      strncpy(out_password, pw.c_str(), pwd_cap - 1);
      out_password[pwd_cap - 1] = '\0';
    }
  }
  p.end();
  return has;
}

static void save_stored(const String &ssid, const String &pwd) {
  Preferences p;
  p.begin(kWifiPrefNs, false);
  p.putString(kKeyWifiSsid, ssid);
  p.putString(kKeyWifiPwd,  pwd);
  p.end();
}

/* ========== AP SSID 生成 ========== */
static String build_ap_ssid(void) {
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char buf[40];
  snprintf(buf, sizeof(buf), "%s%02X%02X%02X",
           kApSsidPrefix, mac[3], mac[4], mac[5]);
  return String(buf);
}

/* ========== 配网页 HTML ========== */
static const char *kConfigHtml = R"HTML(<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>zhy Launcher 配网</title>
<style>
body{font-family:-apple-system,"PingFang SC",sans-serif;background:#f0f2f5;margin:0;padding:1.5rem}
.card{background:#fff;padding:1.5rem;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,0.08);max-width:420px;margin:0 auto}
h2{text-align:center;margin:0 0 1.2rem;color:#2d8a4e}
label{font-weight:600;display:block;margin:0.8rem 0 0.3rem;color:#333}
select,input[type="text"],input[type="password"]{width:100%;padding:0.7rem;font-size:1rem;border:1px solid #ccc;border-radius:8px;box-sizing:border-box}
.row{display:flex;gap:0.5rem;align-items:center}
.row select{flex:1}
button{padding:0.7rem 1rem;font-size:1rem;border:none;border-radius:8px;background:#4a90e2;color:#fff;cursor:pointer}
button.scan{background:#67a6e8;width:auto;padding:0.7rem 0.9rem;white-space:nowrap}
button:active{opacity:0.85}
.submit{width:100%;margin-top:1.2rem;background:#2d8a4e;font-size:1.05rem}
.status{margin-top:0.8rem;color:#888;font-size:0.9rem;text-align:center}
</style></head><body>
<div class="card">
<h2>zhy Launcher 配网</h2>
<label>WiFi 名称</label>
<div class="row">
  <select id="ssid"><option value="">-- 点击右侧扫描 --</option></select>
  <button class="scan" onclick="scan()">扫描</button>
</div>
<label>或手动输入</label>
<input type="text" id="ssid_manual" placeholder="如果扫描不到请在此输入">
<label>WiFi 密码</label>
<input type="password" id="pwd" placeholder="留空表示无密码">
<button class="submit" onclick="save()">保存并连接</button>
<div class="status" id="st"></div>
</div>
<script>
function scan(){
  var st=document.getElementById('st');
  st.textContent='扫描中...';
  fetch('/scan').then(function(r){return r.json();}).then(function(list){
    var sel=document.getElementById('ssid');
    sel.innerHTML='';
    if(list.length===0){st.textContent='未扫描到热点,请手动输入';return;}
    list.forEach(function(a){
      var o=document.createElement('option');
      o.value=a.ssid;o.text=a.ssid+' ('+a.rssi+'dBm)';
      sel.appendChild(o);
    });
    st.textContent='扫描到 '+list.length+' 个热点';
  }).catch(function(e){st.textContent='扫描失败: '+e;});
}
function save(){
  var ssid=document.getElementById('ssid').value;
  var manual=document.getElementById('ssid_manual').value.trim();
  if(manual) ssid=manual;
  if(!ssid){alert('请选择或输入 WiFi 名称');return;}
  var pwd=document.getElementById('pwd').value;
  var st=document.getElementById('st');
  st.textContent='正在提交...';
  var body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pwd);
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
    .then(function(){st.textContent='已提交,设备正在连接,请稍候...';})
    .catch(function(e){st.textContent='提交失败: '+e;});
}
</script>
</body></html>
)HTML";

/* ========== WebServer 路由 ========== */
static void handle_root(void) {
  g_server->send(200, "text/html; charset=utf-8", kConfigHtml);
}

static void handle_scan(void) {
  // AP+STA 模式下扫描，阻塞几秒。前端 async 等待。
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    int rssi = WiFi.RSSI(i);
    // 去重（同名 AP 只保留信号最强的——这里简化为全部列出）
    json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(rssi) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  g_server->send(200, "application/json", json);
}

static void handle_save(void) {
  String ssid = g_server->arg("ssid");
  String pwd  = g_server->arg("password");
  if (ssid.length() == 0) {
    g_server->send(400, "text/plain; charset=utf-8", "missing ssid");
    return;
  }
  save_stored(ssid, pwd);
  g_new_ssid = ssid;
  g_new_pwd  = pwd;
  g_phase    = ConnectPhase::Submitted;
  g_phase_ts = millis();

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;margin:2rem;text-align:center;color:#333}";
  html += "h2{color:#2d8a4e}p{line-height:1.6}</style></head><body>";
  html += "<h2>已收到凭据</h2>";
  html += "<p>SSID: <b>" + ssid + "</b></p>";
  html += "<p>设备正在切换到 STA 模式连接 WiFi...</p>";
  html += "<p>请保持手机连接到目标 WiFi，设备连上后会自动返回主页。</p>";
  html += "</body></html>";
  g_server->send(200, "text/html; charset=utf-8", html);
}

static void start_webserver(void) {
  if (g_server != NULL) return;
  g_server = new WebServer(80);
  g_server->on("/",       HTTP_GET,  handle_root);
  g_server->on("/scan",   HTTP_GET,  handle_scan);
  g_server->on("/save",   HTTP_POST, handle_save);
  g_server->onNotFound([]() { g_server->send(404, "text/plain", "404"); });
  g_server->begin();
}

static void stop_webserver(void) {
  if (g_server != NULL) {
    g_server->stop();
    delete g_server;
    g_server = NULL;
  }
}

/* ========== AP 启停 ========== */
static void start_ap(void) {
  if (g_ap_running) return;
  g_ap_ssid = build_ap_ssid();
  // AP+STA 共存：配网提交后切 STA 时仍可保持 AP 一会儿
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(g_ap_ssid.c_str(), NULL);  // open AP
  delay(100);
  Serial.print("WIFI_CONFIG ap_started ssid=");
  Serial.print(g_ap_ssid);
  Serial.print(" ip=");
  Serial.println(WiFi.softAPIP());
  g_ap_running = true;
}

static void stop_ap(void) {
  if (!g_ap_running) return;
  WiFi.softAPdisconnect(true);
  g_ap_running = false;
  Serial.println("WIFI_CONFIG ap_stopped");
}

/* ========== UI ========== */
static void wifi_back_cb(lv_event_t *e) {
  (void)e;
  launcher_request_return_home();
}

static void update_status_text(const char *text, lv_color_t color) {
  if (g_status_label == NULL) return;
  if (lvgl_port_lock(200)) {
    lv_label_set_text(g_status_label, text);
    lv_obj_set_style_text_color(g_status_label, color, 0);
    lvgl_port_unlock();
  }
}

static void build_ui_locked(void) {
  g_wifi_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(g_wifi_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(g_wifi_scr, 12, 0);
  lv_obj_set_style_pad_row(g_wifi_scr, 6, 0);
  lv_obj_clear_flag(g_wifi_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(g_wifi_scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_wifi_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(g_wifi_scr);
  lv_label_set_text(title, "WiFi 配网");
  lv_obj_set_style_text_font(title, codex_font_20(), 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF7FAFC), 0);

  lv_obj_t *hint1 = lv_label_create(g_wifi_scr);
  lv_label_set_text(hint1, "1. 手机连接此热点:");
  lv_obj_set_style_text_font(hint1, codex_font_16(), 0);
  lv_obj_set_style_text_color(hint1, lv_color_hex(0xC6D1D8), 0);

  g_ap_ssid_label = lv_label_create(g_wifi_scr);
  lv_label_set_text(g_ap_ssid_label, g_ap_ssid.c_str());
  lv_obj_set_style_text_font(g_ap_ssid_label, codex_font_20(), 0);
  lv_obj_set_style_text_color(g_ap_ssid_label, lv_color_hex(0x74B9FF), 0);

  lv_obj_t *hint2 = lv_label_create(g_wifi_scr);
  lv_label_set_text(hint2, "2. 浏览器访问 192.168.4.1");
  lv_obj_set_style_text_font(hint2, codex_font_16(), 0);
  lv_obj_set_style_text_color(hint2, lv_color_hex(0xC6D1D8), 0);

  g_status_label = lv_label_create(g_wifi_scr);
  lv_label_set_text(g_status_label, "等待配网...");
  lv_obj_set_style_text_font(g_status_label, codex_font_16(), 0);
  lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xF0C86F), 0);

  lv_obj_t *btn = lv_btn_create(g_wifi_scr);
  lv_obj_set_size(btn, 100, 30);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, wifi_back_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, "返回");
  lv_obj_center(lbl);
  lv_obj_set_style_text_font(lbl, codex_font_16(), 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xF7FAFC), 0);
}

/* ========== App Lifecycle ========== */
lv_obj_t *app_wifi_config_create(void) {
  // 调用方 launcher_switch_to 已持有 lvgl_port_lock，这里不要二次 lock：
  // lvgl_mux 是 xSemaphoreCreateMutex()（非递归），二次 take 会超时失败导致 return NULL，
  // 表现为点击图标后 app 打不开。其他 app 的 create 也都不自己加锁。
  start_ap();
  start_webserver();
  g_phase    = ConnectPhase::Idle;
  g_phase_ts = millis();

  build_ui_locked();
  return g_wifi_scr;
}

void app_wifi_config_destroy(lv_obj_t *scr) {
  (void)scr;
  stop_webserver();
  stop_ap();
  g_wifi_scr = NULL;
  g_ap_ssid_label = NULL;
  g_status_label = NULL;
  g_back_btn = NULL;
  g_phase = ConnectPhase::Idle;
}

void app_wifi_config_tick(lv_obj_t *scr) {
  (void)scr;
  if (g_server != NULL) g_server->handleClient();

  uint32_t now = millis();

  switch (g_phase) {
    case ConnectPhase::Idle:
      break;

    case ConnectPhase::Submitted: {
      // 给 WebServer 一点时间把响应发完，然后切回 STA
      if (now - g_phase_ts >= kReconnectDelayMs) {
        update_status_text("正在连接 WiFi...", lv_color_hex(0xF0C86F));
        stop_webserver();
        stop_ap();
        WiFi.mode(WIFI_STA);
        WiFi.begin(g_new_ssid.c_str(), g_new_pwd.c_str());
        g_phase = ConnectPhase::Connecting;
        g_phase_ts = now;
      }
      break;
    }

    case ConnectPhase::Connecting: {
      if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        String msg = "已连接 IP:" + ip;
        update_status_text(msg.c_str(), lv_color_hex(0x78F0A4));
        Serial.print("WIFI_CONFIG connected ip=");
        Serial.println(ip);
        g_phase = ConnectPhase::Connected;
        g_phase_ts = now;
      } else if (now - g_phase_ts >= 20000) {
        update_status_text("连接失败,请重试", lv_color_hex(0xFF7E7E));
        Serial.println("WIFI_CONFIG connect_failed");
        g_phase = ConnectPhase::Failed;
        g_phase_ts = now;
      }
      break;
    }

    case ConnectPhase::Connected: {
      if (now - g_phase_ts >= kReturnHomeMs) {
        launcher_request_return_home();
        g_phase = ConnectPhase::Idle;
      }
      break;
    }

    case ConnectPhase::Failed: {
      // 等用户按返回,或 8 秒后自动重新开 AP 让用户再试
      if (now - g_phase_ts >= 8000) {
        start_ap();
        start_webserver();
        update_status_text("等待配网...", lv_color_hex(0xF0C86F));
        g_phase = ConnectPhase::Idle;
      }
      break;
    }
  }
}

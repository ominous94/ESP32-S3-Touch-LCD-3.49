#include "app_imu_test.h"
#include "i2c_bsp.h"
#include <string.h>
#include <math.h>

/* QMI8658 register map */
#define QMI8658_REG_WHOAMI      0x00
#define QMI8658_REG_CTRL1       0x02
#define QMI8658_REG_CTRL2       0x03
#define QMI8658_REG_CTRL3       0x04
#define QMI8658_REG_CTRL7       0x08
#define QMI8658_REG_CTRL9       0x0A
#define QMI8658_REG_STATUS0     0x2E
#define QMI8658_REG_AX_L       0x35
#define QMI8658_REG_GX_L       0x3B
#define QMI8658_REG_RESET       0x60

#define QMI8658_WHOAMI_VAL      0x05
#define QMI8658_RESET_VAL       0xB0

/* QMI8658 ±4g: 4/32768 g/LSB, ±256dps: 256/32768 dps/LSB */
static const float ACCEL_SCALE = 4.0f / 32768.0f;
static const float GYRO_SCALE  = 256.0f / 32768.0f;
static const uint32_t READ_INTERVAL_MS = 100;

/* State */
static lv_obj_t *g_imu_scr = NULL;
static lv_obj_t *g_chip_label = NULL;
static lv_obj_t *g_accel_val[3] = {};
static lv_obj_t *g_gyro_val[3] = {};
static lv_obj_t *g_accel_fill[3] = {};
static lv_obj_t *g_gyro_fill[3] = {};
static uint32_t g_last_read_ms = 0;
static bool g_imu_ok = false;
static uint8_t g_who_am_i = 0;

static const char *AXIS_NAMES[] = {"X", "Y", "Z"};

/* ========== Callbacks ========== */
static void imu_back_cb(lv_event_t *e) {
  (void)e;
  launcher_request_return_home();
}

/* ========== IMU Driver (QMI8658 via i2c_master) ========== */
static bool imu_write_reg(uint8_t reg, uint8_t val) {
  return i2c_write_buff(imu_dev_handle, reg, &val, 1) == 0;
}

static bool imu_read_reg(uint8_t reg, uint8_t *buf, uint8_t len) {
  return i2c_read_buff(imu_dev_handle, reg, buf, len) == 0;
}

static bool imu_init(void) {
  /* Soft reset */
  if (!imu_write_reg(QMI8658_REG_RESET, QMI8658_RESET_VAL))
    return false;
  delay(300);

  /* Verify WHO_AM_I */
  if (!imu_read_reg(QMI8658_REG_WHOAMI, &g_who_am_i, 1))
    return false;
  if (g_who_am_i != QMI8658_WHOAMI_VAL)
    return false;

  /* CTRL1: bit6=ADDR_AI(1) — required for multi-byte burst reads, bit5=BE(0)=little-endian */
  imu_write_reg(QMI8658_REG_CTRL1, 0x40);

  /* CTRL2: Accel — [7]=SelfTest, [6:4]=Range(001=4G), [3:0]=ODR(0011=1000Hz)
     = 0x13 */
  imu_write_reg(QMI8658_REG_CTRL2, 0x13);

  /* CTRL3: Gyro — [7]=SelfTest, [6:4]=Range(100=256DPS), [3:0]=ODR(0011=896.8Hz)
     = 0x43 */
  imu_write_reg(QMI8658_REG_CTRL3, 0x43);

  /* CTRL5: Enable LPF for both accel & gyro to reduce noise */
  imu_write_reg(0x06, 0x11);

  /* CTRL7: enable accel + gyro */
  imu_write_reg(QMI8658_REG_CTRL7, 0x03);

  delay(50);
  return true;
}

static bool imu_read(int16_t accel[3], int16_t gyro[3]) {
  uint8_t status;
  if (!imu_read_reg(QMI8658_REG_STATUS0, &status, 1))
    return false;
  if (!(status & 0x03))
    return false;

  uint8_t buf[6];
  if (status & 0x01) {
    if (!imu_read_reg(QMI8658_REG_AX_L, buf, 6)) return false;
    accel[0] = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    accel[1] = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    accel[2] = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);
  }
  if (status & 0x02) {
    if (!imu_read_reg(QMI8658_REG_GX_L, buf, 6)) return false;
    gyro[0] = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    gyro[1] = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    gyro[2] = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);
  }
  return true;
}

/* ========== UI Builder ========== */
static void build_sensor_panel(lv_obj_t *parent, const char *name,
                                lv_color_t accent,
                                lv_obj_t *vals[3], lv_obj_t *fills[3]) {
  lv_obj_t *panel = lv_obj_create(parent);
  lv_obj_set_size(panel, 314, 136);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x151B20), 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_radius(panel, 6, 0);
  lv_obj_set_style_pad_all(panel, 6, 0);
  lv_obj_set_style_pad_row(panel, 4, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *hdr = lv_label_create(panel);
  lv_label_set_text(hdr, name);
  lv_obj_set_style_text_font(hdr, codex_font_16(), 0);
  lv_obj_set_style_text_color(hdr, accent, 0);

  for (int i = 0; i < 3; i++) {
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, LV_PCT(100), 30);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ax = lv_label_create(row);
    lv_label_set_text(ax, AXIS_NAMES[i]);
    lv_obj_set_style_text_font(ax, codex_font_16(), 0);
    lv_obj_set_style_text_color(ax, lv_color_hex(0x8FA1AD), 0);
    lv_obj_set_width(ax, 14);

    vals[i] = lv_label_create(row);
    lv_label_set_text(vals[i], "+0.000");
    lv_obj_set_style_text_font(vals[i], codex_font_16(), 0);
    lv_obj_set_style_text_color(vals[i], lv_color_hex(0xF7FAFC), 0);
    lv_obj_set_width(vals[i], 60);

    lv_obj_t *bar_bg = lv_obj_create(row);
    lv_obj_set_size(bar_bg, 200, 14);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x1D252C), 0);
    lv_obj_set_style_border_width(bar_bg, 0, 0);
    lv_obj_set_style_radius(bar_bg, 3, 0);
    lv_obj_set_style_pad_all(bar_bg, 2, 0);

    fills[i] = lv_obj_create(bar_bg);
    lv_obj_set_size(fills[i], 0, 10);
    lv_obj_align(fills[i], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(fills[i], accent, 0);
    lv_obj_set_style_border_width(fills[i], 0, 0);
    lv_obj_set_style_radius(fills[i], 2, 0);
  }
}

/* ========== App Lifecycle ========== */
lv_obj_t *app_imu_test_create(void) {
  g_imu_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(g_imu_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_pad_all(g_imu_scr, 4, 0);
  lv_obj_clear_flag(g_imu_scr, LV_OBJ_FLAG_SCROLLABLE);

  g_imu_ok = imu_init();

  /* --- Top bar --- */
  lv_obj_t *top = lv_obj_create(g_imu_scr);
  lv_obj_set_size(top, LV_PCT(100), 28);
  lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(top, 0, 0);
  lv_obj_set_style_pad_all(top, 0, 0);
  lv_obj_set_style_pad_column(top, 8, 0);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(top);
  lv_label_set_text(title, "IMU 测试");
  lv_obj_set_style_text_font(title, codex_font_20(), 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xF7FAFC), 0);

  g_chip_label = lv_label_create(top);
  char chip_buf[24];
  if (g_imu_ok)
    snprintf(chip_buf, sizeof(chip_buf), "QMI8658 0x%02X", g_who_am_i);
  else if (g_who_am_i != 0)
    snprintf(chip_buf, sizeof(chip_buf), "0x%02X", g_who_am_i);
  else
    snprintf(chip_buf, sizeof(chip_buf), "未检测");
  lv_label_set_text(g_chip_label, chip_buf);
  lv_obj_set_style_text_font(g_chip_label, codex_font_16(), 0);
  lv_obj_set_style_text_color(g_chip_label,
    g_imu_ok ? lv_color_hex(0x78F0A4) : lv_color_hex(0xFF7E7E), 0);

  lv_obj_t *spacer = lv_obj_create(top);
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spacer, 0, 0);

  lv_obj_t *btn = lv_btn_create(top);
  lv_obj_set_size(btn, 60, 26);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, imu_back_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "返回");
  lv_obj_center(bl);
  lv_obj_set_style_text_font(bl, codex_font_16(), 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(0xF7FAFC), 0);

  /* --- Two-column content --- */
  lv_obj_t *content = lv_obj_create(g_imu_scr);
  lv_obj_set_size(content, LV_PCT(100), 136);
  lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 32);
  lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(content, 0, 0);
  lv_obj_set_style_pad_all(content, 0, 0);
  lv_obj_set_style_pad_column(content, 4, 0);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);

  build_sensor_panel(content, "加速度 (g)", lv_color_hex(0x78F0A4),
                     g_accel_val, g_accel_fill);
  build_sensor_panel(content, "陀螺仪 (dps)", lv_color_hex(0x74B9FF),
                     g_gyro_val, g_gyro_fill);

  g_last_read_ms = millis();
  return g_imu_scr;
}

void app_imu_test_destroy(lv_obj_t *scr) {
  (void)scr;
  g_imu_scr = NULL;
  g_chip_label = NULL;
  for (int i = 0; i < 3; i++) {
    g_accel_val[i] = NULL;
    g_gyro_val[i] = NULL;
    g_accel_fill[i] = NULL;
    g_gyro_fill[i] = NULL;
  }
}

void app_imu_test_tick(lv_obj_t *scr) {
  (void)scr;
  if (!g_imu_ok || g_imu_scr == NULL) return;

  uint32_t now = millis();
  if (now - g_last_read_ms < READ_INTERVAL_MS) return;
  g_last_read_ms = now;

  int16_t accel[3] = {}, gyro[3] = {};
  if (!imu_read(accel, gyro)) return;

  if (!lvgl_port_lock(-1)) return;

  char buf[16];
  for (int i = 0; i < 3; i++) {
    float ag = accel[i] * ACCEL_SCALE;
    snprintf(buf, sizeof(buf), "%+.3f", ag);
    lv_label_set_text(g_accel_val[i], buf);
    int w = (int)(fabsf(ag) / 4.0f * 192.0f);
    if (w > 192) w = 192;
    if (w < 0) w = 0;
    lv_obj_set_width(g_accel_fill[i], w);
    lv_obj_set_style_bg_color(g_accel_fill[i],
      ag >= 0 ? lv_color_hex(0x78F0A4) : lv_color_hex(0xFF7E7E), 0);

    float gd = gyro[i] * GYRO_SCALE;
    snprintf(buf, sizeof(buf), "%+.1f", gd);
    lv_label_set_text(g_gyro_val[i], buf);
    int gw = (int)(fabsf(gd) / 256.0f * 192.0f);
    if (gw > 192) gw = 192;
    if (gw < 0) gw = 0;
    lv_obj_set_width(g_gyro_fill[i], gw);
    lv_obj_set_style_bg_color(g_gyro_fill[i],
      gd >= 0 ? lv_color_hex(0x74B9FF) : lv_color_hex(0xF0C86F), 0);
  }

  lvgl_port_unlock();
}

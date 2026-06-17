#include "app_ball_roll.h"
#include "i2c_bsp.h"
#include "lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <atomic>

/* QMI8658 寄存器 */
#define QMI_REG_WHOAMI   0x00
#define QMI_REG_CTRL1    0x02
#define QMI_REG_CTRL2    0x03
#define QMI_REG_CTRL3    0x04
#define QMI_REG_CTRL5    0x06
#define QMI_REG_CTRL7    0x08
#define QMI_REG_STATUS0  0x2E
#define QMI_REG_AX_L     0x35
#define QMI_REG_RESET    0x60
#define QMI_WHOAMI_VAL   0x05
#define QMI_RESET_VAL    0xB0

/* ±4g：4/32768 g/LSB */
static const float BR_ACCEL_SCALE = 4.0f / 32768.0f;

/* 竖屏（rotation 0）：172 宽 × 640 高，裁剪到上半屏 320 高 */
static const int BR_SCREEN_W = 172;
static const int BR_SCREEN_H = 320;
static const int BR_CROP_Y_MAX = 320;

/* 物理参数 */
static const float BR_GRAVITY     = 1100.0f;
static const float BR_FRICTION    = 0.992f;
static const float BR_BOUNCE      = 0.55f;
static const float BR_VEL_LIMIT   = 1800.0f;
static const float BR_VEL_EPS     = 4.0f;
static const float BR_ACCEL_ALPHA = 0.30f;
static const int   BR_BALL_RADIUS = 12;
static const uint32_t BR_PHYS_INTERVAL_MS = 10;

/* 状态 */
static lv_obj_t *g_ball_scr   = NULL;
static lv_obj_t *g_ball_obj   = NULL;
static lv_obj_t *g_ball_hint  = NULL;
static bool      g_ball_ready = false;

/* 物理（仅在物理任务里读写） */
static float g_phys_x = 0.0f;
static float g_phys_y = 0.0f;
static float g_phys_vx = 0.0f;
static float g_phys_vy = 0.0f;
static float g_filt_ax = 0.0f;
static float g_filt_ay = 0.0f;
static bool  g_filt_inited = false;

/* 物理→渲染：仅整数像素，原子写入 */
static std::atomic<int32_t> g_render_x{BR_SCREEN_W / 2};
static std::atomic<int32_t> g_render_y{BR_SCREEN_H / 2};
static int g_last_drawn_x = -9999;
static int g_last_drawn_y = -9999;

/* 裁剪延迟启用：让 LVGL 先做几次全屏 flush 把下半屏清干净 */
static uint32_t g_create_ms = 0;
static bool     g_crop_activated = false;
static const uint32_t BR_CROP_DELAY_MS = 400;

static TaskHandle_t g_phys_task = NULL;
static volatile bool g_phys_run = false;

/* ========== 回调 ========== */
static void ball_back_cb(lv_event_t *e) {
  (void)e;
  launcher_request_return_home();
}

/* ========== IMU ========== */
static bool ball_imu_write(uint8_t reg, uint8_t val) {
  return i2c_write_buff(imu_dev_handle, reg, &val, 1) == 0;
}

static bool ball_imu_read(uint8_t reg, uint8_t *buf, uint8_t len) {
  return i2c_read_buff(imu_dev_handle, reg, buf, len) == 0;
}

static bool ball_imu_init(void) {
  if (!ball_imu_write(QMI_REG_RESET, QMI_RESET_VAL)) return false;
  delay(300);

  uint8_t who = 0;
  if (!ball_imu_read(QMI_REG_WHOAMI, &who, 1)) return false;
  if (who != QMI_WHOAMI_VAL) return false;

  ball_imu_write(QMI_REG_CTRL1, 0x40);
  ball_imu_write(QMI_REG_CTRL2, 0x13);
  ball_imu_write(QMI_REG_CTRL3, 0x43);
  ball_imu_write(QMI_REG_CTRL5, 0x11);
  ball_imu_write(QMI_REG_CTRL7, 0x01);
  delay(50);
  return true;
}

static bool ball_imu_read_accel(float *ax, float *ay) {
  uint8_t status = 0;
  if (!ball_imu_read(QMI_REG_STATUS0, &status, 1)) return false;
  if (!(status & 0x01)) return false;

  uint8_t buf[6];
  if (!ball_imu_read(QMI_REG_AX_L, buf, 6)) return false;
  int16_t rx = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
  int16_t ry = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
  *ax = rx * BR_ACCEL_SCALE;
  *ay = ry * BR_ACCEL_SCALE;
  return true;
}

/* ========== 物理任务 ========== */
static void phys_step(float dt) {
  float ax = 0.0f, ay = 0.0f;
  if (!ball_imu_read_accel(&ax, &ay)) return;

  if (!g_filt_inited) {
    g_filt_ax = ax;
    g_filt_ay = ay;
    g_filt_inited = true;
  } else {
    g_filt_ax = g_filt_ax * (1.0f - BR_ACCEL_ALPHA) + ax * BR_ACCEL_ALPHA;
    g_filt_ay = g_filt_ay * (1.0f - BR_ACCEL_ALPHA) + ay * BR_ACCEL_ALPHA;
  }

  /* 重力向量顺时针旋转 90°，再翻转水平方向 */
  float gx =  g_filt_ay;
  float gy =  g_filt_ax;

  g_phys_vx += gx * BR_GRAVITY * dt;
  g_phys_vy += gy * BR_GRAVITY * dt;

  g_phys_vx *= BR_FRICTION;
  g_phys_vy *= BR_FRICTION;

  if (g_phys_vx >  BR_VEL_LIMIT) g_phys_vx =  BR_VEL_LIMIT;
  if (g_phys_vx < -BR_VEL_LIMIT) g_phys_vx = -BR_VEL_LIMIT;
  if (g_phys_vy >  BR_VEL_LIMIT) g_phys_vy =  BR_VEL_LIMIT;
  if (g_phys_vy < -BR_VEL_LIMIT) g_phys_vy = -BR_VEL_LIMIT;

  if (fabsf(g_phys_vx) < BR_VEL_EPS) g_phys_vx = 0.0f;
  if (fabsf(g_phys_vy) < BR_VEL_EPS) g_phys_vy = 0.0f;

  g_phys_x += g_phys_vx * dt;
  g_phys_y += g_phys_vy * dt;

  float min_x = (float)BR_BALL_RADIUS;
  float max_x = (float)(BR_SCREEN_W - BR_BALL_RADIUS);
  float min_y = (float)BR_BALL_RADIUS;
  float max_y = (float)(BR_SCREEN_H - BR_BALL_RADIUS);

  if (g_phys_x < min_x) { g_phys_x = min_x; g_phys_vx = -g_phys_vx * BR_BOUNCE; }
  if (g_phys_x > max_x) { g_phys_x = max_x; g_phys_vx = -g_phys_vx * BR_BOUNCE; }
  if (g_phys_y < min_y) { g_phys_y = min_y; g_phys_vy = -g_phys_vy * BR_BOUNCE; }
  if (g_phys_y > max_y) { g_phys_y = max_y; g_phys_vy = -g_phys_vy * BR_BOUNCE; }

  g_render_x.store((int32_t)(g_phys_x + 0.5f), std::memory_order_relaxed);
  g_render_y.store((int32_t)(g_phys_y + 0.5f), std::memory_order_relaxed);
}

static void ball_phys_task(void *arg) {
  (void)arg;
  TickType_t last = xTaskGetTickCount();
  uint32_t prev_us = micros();
  uint32_t last_render_ms = 0;
  const uint32_t RENDER_INTERVAL_MS = 33;
  while (g_phys_run) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(BR_PHYS_INTERVAL_MS));
    uint32_t now_us = micros();
    float dt = (now_us - prev_us) / 1000000.0f;
    prev_us = now_us;
    if (dt <= 0.0f || dt > 0.05f) dt = BR_PHYS_INTERVAL_MS / 1000.0f;
    phys_step(dt);

    uint32_t now_ms = millis();
    if (now_ms - last_render_ms < RENDER_INTERVAL_MS) continue;

    int rx = g_render_x.load(std::memory_order_relaxed);
    int ry = g_render_y.load(std::memory_order_relaxed);
    if (rx == g_last_drawn_x && ry == g_last_drawn_y) continue;

    if (g_ball_obj == NULL || g_ball_scr == NULL) continue;
    if (lvgl_port_lock(5)) {
      if (g_ball_obj != NULL) {
        lv_obj_set_pos(g_ball_obj,
                       (lv_coord_t)(rx - BR_BALL_RADIUS),
                       (lv_coord_t)(ry - BR_BALL_RADIUS));
      }
      lvgl_port_unlock();
      g_last_drawn_x = rx;
      g_last_drawn_y = ry;
      last_render_ms = now_ms;
    }
  }
  g_phys_task = NULL;
  vTaskDelete(NULL);
}

/* ========== 应用 ========== */
lv_obj_t *app_ball_roll_create(void) {
  /* 切到无旋转：flush_cb 跳过 lv_draw_sw_rotate，省 ~10ms/帧 */
  lv_display_t *disp = lv_display_get_default();
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);

  g_ball_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(g_ball_scr, lv_color_hex(0x0E1116), 0);
  lv_obj_set_style_pad_all(g_ball_scr, 0, 0);
  lv_obj_clear_flag(g_ball_scr, LV_OBJ_FLAG_SCROLLABLE);

  g_ball_ready = ball_imu_init();

  /* 边界框：标识小球活动区域 172×320 */
  lv_obj_t *border = lv_obj_create(g_ball_scr);
  lv_obj_set_size(border, BR_SCREEN_W, BR_SCREEN_H);
  lv_obj_set_pos(border, 0, 0);
  lv_obj_clear_flag(border, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(border, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(border, 2, 0);
  lv_obj_set_style_border_color(border, lv_color_hex(0x78F0A4), 0);
  lv_obj_set_style_radius(border, 0, 0);
  lv_obj_set_style_pad_all(border, 0, 0);

  /* 返回按钮 */
  lv_obj_t *btn = lv_btn_create(g_ball_scr);
  lv_obj_set_size(btn, 56, 26);
  lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 6, 6);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
  lv_obj_set_style_radius(btn, 6, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_add_event_cb(btn, ball_back_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_t *bl = lv_label_create(btn);
  lv_label_set_text(bl, "返回");
  lv_obj_center(bl);
  lv_obj_set_style_text_font(bl, codex_font_16(), 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(0xF7FAFC), 0);

  if (!g_ball_ready) {
    g_ball_hint = lv_label_create(g_ball_scr);
    lv_label_set_text(g_ball_hint, "IMU 未检测到");
    lv_obj_set_style_text_font(g_ball_hint, codex_font_16(), 0);
    lv_obj_set_style_text_color(g_ball_hint, lv_color_hex(0xFF7E7E), 0);
    lv_obj_align(g_ball_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
  }

  g_ball_obj = lv_obj_create(g_ball_scr);
  lv_obj_set_size(g_ball_obj, BR_BALL_RADIUS * 2, BR_BALL_RADIUS * 2);
  lv_obj_clear_flag(g_ball_obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(g_ball_obj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_ball_obj, lv_color_hex(0x78F0A4), 0);
  lv_obj_set_style_border_width(g_ball_obj, 0, 0);
  lv_obj_set_style_shadow_width(g_ball_obj, 0, 0);

  g_phys_x = BR_SCREEN_W * 0.5f;
  g_phys_y = BR_SCREEN_H * 0.5f;
  g_phys_vx = 0.0f;
  g_phys_vy = 0.0f;
  g_filt_inited = false;
  g_render_x.store((int32_t)g_phys_x, std::memory_order_relaxed);
  g_render_y.store((int32_t)g_phys_y, std::memory_order_relaxed);
  g_last_drawn_x = -9999;
  g_last_drawn_y = -9999;

  lv_obj_set_pos(g_ball_obj,
                 (lv_coord_t)(g_phys_x - BR_BALL_RADIUS),
                 (lv_coord_t)(g_phys_y - BR_BALL_RADIUS));

  g_create_ms = millis();
  g_crop_activated = false;

  if (g_ball_ready && g_phys_task == NULL) {
    g_phys_run = true;
    xTaskCreatePinnedToCore(ball_phys_task, "ball_phys", 3 * 1024, NULL, 1, &g_phys_task, 1);
  }

  return g_ball_scr;
}

void app_ball_roll_destroy(lv_obj_t *scr) {
  (void)scr;
  if (g_phys_task != NULL) {
    g_phys_run = false;
    for (int i = 0; i < 20 && g_phys_task != NULL; ++i) delay(5);
  }

  /* 关闭裁剪，恢复旋转 270 */
  lvgl_port_set_crop(false, 0);
  g_crop_activated = false;

  lv_display_t *disp = lv_display_get_default();
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

  g_ball_scr = NULL;
  g_ball_obj = NULL;
  g_ball_hint = NULL;
}

void app_ball_roll_tick(lv_obj_t *scr) {
  (void)scr;
  /* 延迟启用裁剪：先让 LVGL 做几次全屏 flush 清空下半屏 */
  if (!g_crop_activated && g_ball_ready) {
    uint32_t elapsed = millis() - g_create_ms;
    if (elapsed >= BR_CROP_DELAY_MS) {
      lvgl_port_set_crop(true, BR_CROP_Y_MAX);
      g_crop_activated = true;
    }
  }
}

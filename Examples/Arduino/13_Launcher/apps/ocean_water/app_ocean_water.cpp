#include "app_ocean_water.h"
#include "ocean_flip.h"
#include "i2c_bsp.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "core/lv_obj_event_private.h"
#include <math.h>
#include <string.h>

namespace {

constexpr int OCEAN_LEVEL_MAX = 20;
constexpr uint32_t OCEAN_FRAME_MS = 33;
constexpr float OCEAN_ACCEL_SCALE = 4.0f / 32768.0f;
constexpr float OCEAN_GRAVITY_ALPHA = 0.06f;
constexpr float OCEAN_SHAKE_ALPHA = 0.45f;
constexpr float OCEAN_SHAKE_GAIN = 1.60f;
constexpr float OCEAN_SHAKE_DEADZONE_G = 0.04f;
constexpr float OCEAN_EFFECTIVE_ACCEL_MAX_G = 2.50f;
constexpr float OCEAN_GYRO_SCALE_RAD_S = (1024.0f / 32768.0f) * (PI / 180.0f);
constexpr float OCEAN_GYRO_ALPHA = 0.35f;
constexpr float OCEAN_ANGULAR_ACCEL_ALPHA = 0.25f;
constexpr float OCEAN_GYRO_DEADZONE_RAD_S = 2.0f * (PI / 180.0f);
constexpr float OCEAN_ANGULAR_SPEED_MAX_RAD_S = 6.0f;
constexpr float OCEAN_ANGULAR_ACCEL_MAX_RAD_S2 = 30.0f;
constexpr uint32_t OCEAN_PROFILE_FRAMES = 120;
constexpr uint32_t OCEAN_IDLE_WAVE_GAP_MIN_MS = 4000;
constexpr uint32_t OCEAN_IDLE_WAVE_GAP_MAX_MS = 9000;
constexpr float OCEAN_IDLE_WAVE_BASE_MIN = 0.85f;
constexpr float OCEAN_IDLE_WAVE_BASE_MAX = 1.35f;
constexpr float OCEAN_STILL_ACCEL_DELTA_G = 0.055f;
constexpr int OCEAN_THEME_COUNT = 4;

struct OceanConfig {
  const char *mode_name;
  int grid_w;
  int grid_h;
  int cell_size;
  int cell_draw_size;
  int pressure_iters;
};

struct OceanStats {
  bool ready;
  uint32_t frames;
  uint64_t started_us;
  uint64_t elapsed_us;
  uint64_t imu_us;
  uint64_t flip_us;
  uint64_t draw_us;
};

struct OceanIdleWaveState {
  bool motion_reference_ready;
  bool idle_ready;
  float last_accel[3];
  float still_accel[3];
  uint32_t still_since_ms;
  uint32_t next_wave_ms;
};

struct OceanUserSettings {
  uint32_t idle_seconds;
  uint8_t wave_strength;
  uint8_t theme_index;
  bool random_waves;
};

struct OceanColorStop {
  float position;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static const OceanConfig OCEAN_CONFIG_STANDARD = {
  "standard", 8, 32, 20, 19, 10,
};

static const OceanColorStop OCEAN_PALETTES[OCEAN_THEME_COUNT][6] = {
  {
    {0.00f,   0,   0,   0}, {0.12f,   0,   8,  28},
    {0.35f,   0,  35, 105}, {0.62f,   0, 100, 185},
    {0.82f,  18, 185, 228}, {1.00f, 170, 246, 255},
  },
  {
    {0.00f,   0,   0,   0}, {0.12f,   0,  14,  18},
    {0.35f,   0,  55,  52}, {0.62f,   0, 130, 102},
    {0.82f,  70, 210, 155}, {1.00f, 195, 255, 220},
  },
  {
    {0.00f,   0,   0,   0}, {0.12f,  12,   4,  30},
    {0.35f,  48,  20, 110}, {0.62f, 110,  50, 190},
    {0.82f, 190, 100, 235}, {1.00f, 245, 215, 255},
  },
  {
    {0.00f,   0,   0,   0}, {0.12f,  24,   5,   8},
    {0.35f, 100,  20,  35}, {0.62f, 190,  55,  55},
    {0.82f, 245, 130,  65}, {1.00f, 255, 235, 170},
  },
};

static const uint32_t OCEAN_THEME_PREVIEWS[OCEAN_THEME_COUNT] = {
  0x12B9E8, 0x33D19A, 0xB46CFF, 0xFF7A59,
};

constexpr uint8_t OCEAN_QMI_REG_WHOAMI = 0x00;
constexpr uint8_t OCEAN_QMI_REG_CTRL1 = 0x02;
constexpr uint8_t OCEAN_QMI_REG_CTRL2 = 0x03;
constexpr uint8_t OCEAN_QMI_REG_CTRL3 = 0x04;
constexpr uint8_t OCEAN_QMI_REG_CTRL5 = 0x06;
constexpr uint8_t OCEAN_QMI_REG_CTRL7 = 0x08;
constexpr uint8_t OCEAN_QMI_REG_STATUS0 = 0x2E;
constexpr uint8_t OCEAN_QMI_REG_AX_L = 0x35;
constexpr uint8_t OCEAN_QMI_REG_GZ_L = 0x3F;
constexpr uint8_t OCEAN_QMI_REG_RESET = 0x60;
constexpr uint8_t OCEAN_QMI_WHOAMI_VALUE = 0x05;

static lv_obj_t *g_ocean_scr = nullptr;
static TaskHandle_t g_sim_task = nullptr;
static SemaphoreHandle_t g_task_done = nullptr;
static volatile bool g_sim_running = false;
static bool g_direct_mode_active = false;
static const OceanConfig *g_config = nullptr;
static OceanStats g_stats = {};
static OceanUserSettings g_user_settings = {10, 5, 0, true};
static uint16_t g_palette[OCEAN_LEVEL_MAX + 1] = {};
static lv_obj_t *g_config_panel = nullptr;
static lv_obj_t *g_idle_value_label = nullptr;
static lv_obj_t *g_strength_value_label = nullptr;
static lv_obj_t *g_random_value_label = nullptr;
static lv_obj_t *g_enter_button = nullptr;
static lv_obj_t *g_theme_buttons[OCEAN_THEME_COUNT] = {};

static bool imu_write(uint8_t reg, uint8_t value)
{
  return i2c_write_buff(imu_dev_handle, reg, &value, 1) == 0;
}

static bool imu_read(uint8_t reg, uint8_t *buffer, uint8_t length)
{
  return i2c_read_buff(imu_dev_handle, reg, buffer, length) == 0;
}

static bool ocean_imu_init()
{
  if (!imu_write(OCEAN_QMI_REG_RESET, 0xB0)) return false;
  delay(300);

  uint8_t who_am_i = 0;
  if (!imu_read(OCEAN_QMI_REG_WHOAMI, &who_am_i, 1) || who_am_i != OCEAN_QMI_WHOAMI_VALUE) return false;

  // 自动递增地址、小端；加速度 ±4g，陀螺仪 ±1024dps，并启用低通滤波。
  if (!imu_write(OCEAN_QMI_REG_CTRL1, 0x40)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL2, 0x13)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL3, 0x63)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL5, 0x11)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL7, 0x03)) return false;
  delay(50);
  return true;
}

static bool ocean_read_motion(float *gx, float *gy, float *gz,
                              float *angular_velocity, bool *gyro_sample_ready)
{
  uint8_t status = 0;
  if (!imu_read(OCEAN_QMI_REG_STATUS0, &status, 1) || !(status & 0x01)) return false;

  uint8_t data[6];
  if (!imu_read(OCEAN_QMI_REG_AX_L, data, sizeof(data))) return false;
  int16_t raw_x = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
  int16_t raw_y = (int16_t)(((uint16_t)data[3] << 8) | data[2]);
  int16_t raw_z = (int16_t)(((uint16_t)data[5] << 8) | data[4]);
  float ax = (float)raw_x * OCEAN_ACCEL_SCALE;
  float ay = (float)raw_y * OCEAN_ACCEL_SCALE;
  float az = (float)raw_z * OCEAN_ACCEL_SCALE;

  // QMI8658 轴到 rotation 0 屏幕坐标的映射。
  *gx = ay;
  *gy = ax;
  *gz = az;

  *gyro_sample_ready = false;
  if (status & 0x02) {
    uint8_t gyro_data[2];
    if (imu_read(OCEAN_QMI_REG_GZ_L, gyro_data, sizeof(gyro_data))) {
      const int16_t raw_gz = (int16_t)(((uint16_t)gyro_data[1] << 8) | gyro_data[0]);
      // 屏幕 X/Y 映射交换了传感器 X/Y，法向轴方向因此与传感器 Z 相反。
      *angular_velocity = -(float)raw_gz * OCEAN_GYRO_SCALE_RAD_S;
      *gyro_sample_ready = true;
    }
  }
  return true;
}

static float apply_accel_deadzone(float value)
{
  const float magnitude = fabsf(value);
  if (magnitude <= OCEAN_SHAKE_DEADZONE_G) return 0.0f;
  return copysignf(magnitude - OCEAN_SHAKE_DEADZONE_G, value);
}

static void clamp_accel_vector(float *x, float *y)
{
  const float length = sqrtf(*x * *x + *y * *y);
  if (length > OCEAN_EFFECTIVE_ACCEL_MAX_G && isfinite(length)) {
    const float scale = OCEAN_EFFECTIVE_ACCEL_MAX_G / length;
    *x *= scale;
    *y *= scale;
  }
}

static float apply_gyro_deadzone(float value)
{
  const float magnitude = fabsf(value);
  if (magnitude <= OCEAN_GYRO_DEADZONE_RAD_S) return 0.0f;
  return copysignf(magnitude - OCEAN_GYRO_DEADZONE_RAD_S, value);
}

static float clamp_signed(float value, float limit)
{
  if (value > limit) return limit;
  if (value < -limit) return -limit;
  return value;
}

static float random_float(float min_value, float max_value)
{
  const float unit = (float)esp_random() / (float)UINT32_MAX;
  return min_value + (max_value - min_value) * unit;
}

static uint32_t random_ms(uint32_t min_value, uint32_t max_value)
{
  if (max_value <= min_value) return min_value;
  return min_value + esp_random() % (max_value - min_value + 1U);
}

static bool time_reached(uint32_t now_ms, uint32_t target_ms)
{
  return (int32_t)(now_ms - target_ms) >= 0;
}

static void reset_idle_wave(OceanIdleWaveState *state, uint32_t now_ms,
                            float ax, float ay, float az)
{
  state->motion_reference_ready = true;
  state->idle_ready = false;
  state->last_accel[0] = state->still_accel[0] = ax;
  state->last_accel[1] = state->still_accel[1] = ay;
  state->last_accel[2] = state->still_accel[2] = az;
  state->still_since_ms = now_ms;
  state->next_wave_ms = 0;
}

static bool update_stationary_state(OceanIdleWaveState *state, uint32_t now_ms,
                                    float ax, float ay, float az)
{
  if (!state->motion_reference_ready) {
    reset_idle_wave(state, now_ms, ax, ay, az);
    return true;
  }

  const float sample_dx = ax - state->last_accel[0];
  const float sample_dy = ay - state->last_accel[1];
  const float sample_dz = az - state->last_accel[2];
  const float anchor_dx = ax - state->still_accel[0];
  const float anchor_dy = ay - state->still_accel[1];
  const float anchor_dz = az - state->still_accel[2];
  const float threshold_sq = OCEAN_STILL_ACCEL_DELTA_G * OCEAN_STILL_ACCEL_DELTA_G;
  const float sample_delta_sq = sample_dx * sample_dx + sample_dy * sample_dy + sample_dz * sample_dz;
  const float anchor_delta_sq = anchor_dx * anchor_dx + anchor_dy * anchor_dy + anchor_dz * anchor_dz;

  state->last_accel[0] = ax;
  state->last_accel[1] = ay;
  state->last_accel[2] = az;
  if (sample_delta_sq > threshold_sq || anchor_delta_sq > threshold_sq) {
    reset_idle_wave(state, now_ms, ax, ay, az);
    return false;
  }
  return true;
}

static bool should_start_idle_wave(OceanIdleWaveState *state, uint32_t now_ms,
                                   uint32_t idle_delay_ms)
{
  if (!state->motion_reference_ready) return false;

  if (!state->idle_ready) {
    if ((uint32_t)(now_ms - state->still_since_ms) < idle_delay_ms) return false;
    state->idle_ready = true;
    const uint32_t gap_ms = random_ms(OCEAN_IDLE_WAVE_GAP_MIN_MS,
                                      OCEAN_IDLE_WAVE_GAP_MAX_MS);
    state->next_wave_ms = now_ms + gap_ms;
    return true;
  }

  if (!time_reached(now_ms, state->next_wave_ms)) return false;
  const uint32_t gap_ms = random_ms(OCEAN_IDLE_WAVE_GAP_MIN_MS,
                                    OCEAN_IDLE_WAVE_GAP_MAX_MS);
  state->next_wave_ms = now_ms + gap_ms;
  return true;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((uint16_t)(r & 0xF8) << 8) |
                    ((uint16_t)(g & 0xFC) << 3) |
                    ((uint16_t)b >> 3));
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, float t)
{
  return (uint8_t)lrintf((float)a + ((float)b - (float)a) * t);
}

static void build_palette(uint8_t theme_index)
{
  if (theme_index >= OCEAN_THEME_COUNT) theme_index = 0;
  const OceanColorStop *stops = OCEAN_PALETTES[theme_index];

  for (int level = 0; level <= OCEAN_LEVEL_MAX; ++level) {
    float t = (float)level / (float)OCEAN_LEVEL_MAX;
    const OceanColorStop *a = &stops[0];
    const OceanColorStop *b = &stops[1];
    for (size_t i = 1; i < 6; ++i) {
      if (t <= stops[i].position) {
        a = &stops[i - 1];
        b = &stops[i];
        break;
      }
    }
    float span = b->position - a->position;
    float local_t = span > 0.0f ? (t - a->position) / span : 0.0f;
    if (local_t < 0.0f) local_t = 0.0f;
    if (local_t > 1.0f) local_t = 1.0f;
    g_palette[level] = rgb565(lerp_u8(a->r, b->r, local_t),
                              lerp_u8(a->g, b->g, local_t),
                              lerp_u8(a->b, b->b, local_t));
  }
}

static void map_grid_colors(const OceanConfig *config, const float *grid, uint16_t *colors)
{
  for (int y = 0; y < config->grid_h; ++y) {
    for (int x = 0; x < config->grid_w; ++x) {
      float value = grid[x * config->grid_h + y];
      if (!isfinite(value) || value < 0.0f) value = 0.0f;
      if (value > (float)OCEAN_LEVEL_MAX) value = (float)OCEAN_LEVEL_MAX;
      int level = (int)lrintf(value);
      colors[y * config->grid_w + x] = g_palette[level];
    }
  }
}

static void ocean_sim_task(void *arg)
{
  (void)arg;
  const OceanConfig *config = g_config;
  const OceanUserSettings settings = g_user_settings;
  if (config == nullptr) {
    Serial.println("OCEAN init_failed reason=config");
    g_sim_task = nullptr;
    if (g_task_done != nullptr) xSemaphoreGive(g_task_done);
    vTaskDelete(nullptr);
    return;
  }

  const size_t grid_count = (size_t)config->grid_w * (size_t)config->grid_h;
  float *grid = (float *)heap_caps_calloc(grid_count, sizeof(float),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint16_t *colors = (uint16_t *)heap_caps_calloc(grid_count, sizeof(uint16_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const float sim_w = 1.0f;
  const float sim_h = sim_w * ((float)(config->grid_h + 1) / (float)(config->grid_w + 1));
  OceanFlipFluid *fluid = nullptr;
  if (grid != nullptr && colors != nullptr) {
    fluid = ocean_flip_create(sim_w, sim_h, config->grid_w, config->grid_h, 0.60f);
  }
  const bool imu_ready = ocean_imu_init();
  float gravity_gx = 0.0f;
  float gravity_gy = 1.0f;
  float shake_gx = 0.0f;
  float shake_gy = 0.0f;
  float angular_velocity = 0.0f;
  float angular_acceleration = 0.0f;
  bool gravity_initialized = false;
  bool gyro_initialized = false;
  OceanIdleWaveState idle_wave = {};
  memset(&g_stats, 0, sizeof(g_stats));
  Serial.println(imu_ready ? "OCEAN imu_ready chip=QMI8658" : "OCEAN imu_failed fallback=synthetic");

  if (fluid == nullptr) {
    Serial.println("OCEAN init_failed reason=simulation_alloc");
  } else {
    ocean_flip_set_gravity_scale(fluid, 9.81f);
    ocean_flip_set_solver_quality(fluid, 1, config->pressure_iters, 0.90f);
    Serial.printf("OCEAN simulation_ready mode=%s grid=%dx%d pressure=%d fps=30\n",
                  config->mode_name, config->grid_w, config->grid_h, config->pressure_iters);

    const float dt = 1.0f / 30.0f;
    const TickType_t target_frame_ticks = pdMS_TO_TICKS(OCEAN_FRAME_MS);
    g_stats.started_us = esp_timer_get_time();
    while (g_sim_running) {
      TickType_t frame_started_ticks = xTaskGetTickCount();
      const bool profile_this_frame = g_stats.frames < OCEAN_PROFILE_FRAMES;
      uint64_t stage_started_us = profile_this_frame ? esp_timer_get_time() : 0;
      float gx = gravity_gx + shake_gx * OCEAN_SHAKE_GAIN;
      float gy = gravity_gy + shake_gy * OCEAN_SHAKE_GAIN;
      const uint32_t now_ms = millis();
      if (imu_ready) {
        float measured_gx = 0.0f;
        float measured_gy = 0.0f;
        float measured_gz = 0.0f;
        float measured_angular_velocity = 0.0f;
        bool gyro_sample_ready = false;
        if (ocean_read_motion(&measured_gx, &measured_gy, &measured_gz,
                              &measured_angular_velocity, &gyro_sample_ready)) {
          if (settings.random_waves) {
            update_stationary_state(&idle_wave, now_ms, measured_gx, measured_gy, measured_gz);
          }
          if (!gravity_initialized) {
            gravity_gx = measured_gx;
            gravity_gy = measured_gy;
            shake_gx = 0.0f;
            shake_gy = 0.0f;
            gravity_initialized = true;
          } else {
            gravity_gx += (measured_gx - gravity_gx) * OCEAN_GRAVITY_ALPHA;
            gravity_gy += (measured_gy - gravity_gy) * OCEAN_GRAVITY_ALPHA;

            const float raw_shake_x = apply_accel_deadzone(measured_gx - gravity_gx);
            const float raw_shake_y = apply_accel_deadzone(measured_gy - gravity_gy);
            shake_gx += (raw_shake_x - shake_gx) * OCEAN_SHAKE_ALPHA;
            shake_gy += (raw_shake_y - shake_gy) * OCEAN_SHAKE_ALPHA;
          }
          gx = gravity_gx + shake_gx * OCEAN_SHAKE_GAIN;
          gy = gravity_gy + shake_gy * OCEAN_SHAKE_GAIN;
          clamp_accel_vector(&gx, &gy);

          if (gyro_sample_ready) {
            measured_angular_velocity = clamp_signed(
                apply_gyro_deadzone(measured_angular_velocity),
                OCEAN_ANGULAR_SPEED_MAX_RAD_S);
            if (!gyro_initialized) {
              angular_velocity = measured_angular_velocity;
              angular_acceleration = 0.0f;
              gyro_initialized = true;
            } else {
              const float previous_velocity = angular_velocity;
              angular_velocity += (measured_angular_velocity - angular_velocity) * OCEAN_GYRO_ALPHA;
              const float measured_angular_acceleration = clamp_signed(
                  (angular_velocity - previous_velocity) / dt,
                  OCEAN_ANGULAR_ACCEL_MAX_RAD_S2);
              angular_acceleration +=
                  (measured_angular_acceleration - angular_acceleration) * OCEAN_ANGULAR_ACCEL_ALPHA;
            }
          } else {
            angular_velocity *= 1.0f - OCEAN_GYRO_ALPHA;
            angular_acceleration *= 1.0f - OCEAN_ANGULAR_ACCEL_ALPHA;
          }
        } else {
          shake_gx *= 1.0f - OCEAN_SHAKE_ALPHA;
          shake_gy *= 1.0f - OCEAN_SHAKE_ALPHA;
          angular_velocity *= 1.0f - OCEAN_GYRO_ALPHA;
          angular_acceleration *= 1.0f - OCEAN_ANGULAR_ACCEL_ALPHA;
          gx = gravity_gx + shake_gx * OCEAN_SHAKE_GAIN;
          gy = gravity_gy + shake_gy * OCEAN_SHAKE_GAIN;
          clamp_accel_vector(&gx, &gy);
        }
      } else {
        if (settings.random_waves && !idle_wave.motion_reference_ready) {
          reset_idle_wave(&idle_wave, now_ms, 0.0f, 1.0f, 0.0f);
        }
        gx = 0.0f;
        gy = 1.0f;
      }
      if (settings.random_waves &&
          should_start_idle_wave(&idle_wave, now_ms, settings.idle_seconds * 1000UL)) {
        const float direction = (esp_random() & 1U) ? 1.0f : -1.0f;
        const float position = random_float(0.10f, 0.90f);
        const float strength = random_float(OCEAN_IDLE_WAVE_BASE_MIN,
                                            OCEAN_IDLE_WAVE_BASE_MAX) *
                               (float)settings.wave_strength;
        ocean_flip_apply_wave_impulse(fluid, gx, gy, position, direction, strength);
      }
      if (profile_this_frame) g_stats.imu_us += esp_timer_get_time() - stage_started_us;

      if (profile_this_frame) stage_started_us = esp_timer_get_time();
      ocean_flip_step_rotating(fluid, dt, gx, gy,
                               angular_velocity, angular_acceleration);
      ocean_flip_get_led_grid(fluid, grid, config->grid_w, config->grid_h);
      map_grid_colors(config, grid, colors);
      if (profile_this_frame) g_stats.flip_us += esp_timer_get_time() - stage_started_us;

      uint32_t draw_us = 0;
      if (!lvgl_port_direct_draw_grid(colors,
                                      config->grid_w, config->grid_h,
                                      config->cell_size, config->cell_draw_size,
                                      (172 - config->grid_w * config->cell_size) / 2,
                                      profile_this_frame ? &draw_us : nullptr)) {
        Serial.println("OCEAN direct_draw_failed");
      }
      if (profile_this_frame) {
        g_stats.draw_us += draw_us;
        g_stats.frames++;
        if (g_stats.frames == OCEAN_PROFILE_FRAMES) {
          g_stats.elapsed_us = esp_timer_get_time() - g_stats.started_us;
          g_stats.ready = true;
        }
      }
      TickType_t frame_used_ticks = xTaskGetTickCount() - frame_started_ticks;
      if (frame_used_ticks < target_frame_ticks) {
        vTaskDelay(target_frame_ticks - frame_used_ticks);
      }
    }
    ocean_flip_destroy(fluid);
  }
  if (grid != nullptr) heap_caps_free(grid);
  if (colors != nullptr) heap_caps_free(colors);

  g_sim_task = nullptr;
  if (g_task_done != nullptr) xSemaphoreGive(g_task_done);
  vTaskDelete(nullptr);
}

static void stop_simulation()
{
  if (g_sim_task == nullptr) return;
  g_sim_running = false;
  if (g_task_done != nullptr && xSemaphoreTake(g_task_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("OCEAN stop_timeout");
    vTaskDelete(g_sim_task);
    g_sim_task = nullptr;
  }
}

static const char *ocean_ui_text(const char *zh, const char *en)
{
  return ttf_font_16 != nullptr ? zh : en;
}

static lv_obj_t *create_ui_label(lv_obj_t *parent, const char *text,
                                 int x, int y, int width,
                                 lv_color_t color, bool large = false)
{
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_font(label, large ? codex_font_20() : codex_font_16(), 0);
  lv_obj_set_style_text_color(label, color, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  return label;
}

static lv_obj_t *create_setting_card(int x, int width)
{
  lv_obj_t *card = lv_obj_create(g_config_panel);
  lv_obj_set_size(card, width, 110);
  lv_obj_set_pos(card, x, 50);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x182128), 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x2A3740), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  return card;
}

static void ocean_slider_hit_test_cb(lv_event_t *event)
{
  lv_hit_test_info_t *info = lv_event_get_hit_test_info(event);
  if (info != nullptr) info->res = true;
}

static void style_ocean_slider(lv_obj_t *slider, lv_color_t accent)
{
  lv_obj_set_height(slider, 24);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A3740), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, accent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(0xF7FAFC), LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, ocean_slider_hit_test_cb, LV_EVENT_HIT_TEST, nullptr);
}

static void refresh_idle_value_label()
{
  if (g_idle_value_label == nullptr) return;
  char text[20];
  if (ttf_font_16 != nullptr) snprintf(text, sizeof(text), "%lu秒", (unsigned long)g_user_settings.idle_seconds);
  else snprintf(text, sizeof(text), "%lus", (unsigned long)g_user_settings.idle_seconds);
  lv_label_set_text(g_idle_value_label, text);
}

static void idle_time_changed_cb(lv_event_t *event)
{
  g_user_settings.idle_seconds = (uint32_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event));
  refresh_idle_value_label();
}

static void refresh_strength_value_label()
{
  if (g_strength_value_label == nullptr) return;
  char text[12];
  snprintf(text, sizeof(text), "%ux", (unsigned)g_user_settings.wave_strength);
  lv_label_set_text(g_strength_value_label, text);
}

static void wave_strength_changed_cb(lv_event_t *event)
{
  g_user_settings.wave_strength = (uint8_t)lv_slider_get_value((lv_obj_t *)lv_event_get_target(event));
  refresh_strength_value_label();
}

static void refresh_theme_buttons()
{
  for (int i = 0; i < OCEAN_THEME_COUNT; ++i) {
    if (g_theme_buttons[i] == nullptr) continue;
    const bool selected = i == g_user_settings.theme_index;
    lv_obj_set_style_border_width(g_theme_buttons[i], selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(g_theme_buttons[i],
                                  selected ? lv_color_hex(0xF7FAFC) : lv_color_hex(0x46545E), 0);
  }
}

static void theme_selected_cb(lv_event_t *event)
{
  int index = (int)(intptr_t)lv_event_get_user_data(event);
  if (index < 0 || index >= OCEAN_THEME_COUNT) return;
  g_user_settings.theme_index = (uint8_t)index;
  refresh_theme_buttons();
}

static void random_wave_changed_cb(lv_event_t *event)
{
  lv_obj_t *control = (lv_obj_t *)lv_event_get_target(event);
  g_user_settings.random_waves = lv_obj_has_state(control, LV_STATE_CHECKED);
  if (g_random_value_label != nullptr) {
    lv_label_set_text(g_random_value_label,
                      g_user_settings.random_waves ? ocean_ui_text("开启", "On")
                                                   : ocean_ui_text("关闭", "Off"));
    lv_obj_set_style_text_color(g_random_value_label,
                                g_user_settings.random_waves ? lv_color_hex(0x78F0A4)
                                                             : lv_color_hex(0x8A99A5), 0);
  }
}

static bool start_simulation_from_config()
{
  if (g_config == nullptr || g_sim_task != nullptr || g_direct_mode_active) return false;

  lvgl_port_set_crop(false, 0);
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_0);
  g_direct_mode_active = lvgl_port_direct_mode_begin();
  if (!g_direct_mode_active) {
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
    Serial.println("OCEAN init_failed reason=direct_mode");
    return false;
  }

  build_palette(g_user_settings.theme_index);
  g_task_done = xSemaphoreCreateBinary();
  if (g_task_done == nullptr) {
    lvgl_port_direct_mode_end();
    g_direct_mode_active = false;
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
    Serial.println("OCEAN init_failed reason=semaphore");
    return false;
  }

  memset(&g_stats, 0, sizeof(g_stats));
  g_sim_running = true;
  BaseType_t created = xTaskCreatePinnedToCore(ocean_sim_task, "ocean_sim", 8 * 1024,
                                               nullptr, 1, &g_sim_task, 1);
  if (created != pdPASS) {
    g_sim_running = false;
    g_sim_task = nullptr;
    vSemaphoreDelete(g_task_done);
    g_task_done = nullptr;
    lvgl_port_direct_mode_end();
    g_direct_mode_active = false;
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
    Serial.println("OCEAN init_failed reason=task_create");
    return false;
  }

  Serial.printf("OCEAN app_started renderer=direct_dma mode=%s idle=%lus strength=%ux theme=%u random=%u\n",
                g_config->mode_name, (unsigned long)g_user_settings.idle_seconds,
                (unsigned)g_user_settings.wave_strength, (unsigned)g_user_settings.theme_index,
                g_user_settings.random_waves ? 1U : 0U);
  return true;
}

static void enter_ocean_cb(lv_event_t *event)
{
  (void)event;
  if (g_config_panel == nullptr || g_enter_button == nullptr) return;
  lv_obj_add_state(g_enter_button, LV_STATE_DISABLED);
  lv_obj_add_flag(g_config_panel, LV_OBJ_FLAG_HIDDEN);
  if (!start_simulation_from_config()) {
    lv_obj_clear_flag(g_config_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(g_enter_button, LV_STATE_DISABLED);
  }
}

static void back_to_launcher_cb(lv_event_t *event)
{
  (void)event;
  launcher_request_return_home();
}

static void build_config_ui()
{
  g_config_panel = lv_obj_create(g_ocean_scr);
  lv_obj_set_size(g_config_panel, 640, 172);
  lv_obj_set_pos(g_config_panel, 0, 0);
  lv_obj_clear_flag(g_config_panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(g_config_panel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_config_panel, 0, 0);
  lv_obj_set_style_pad_all(g_config_panel, 0, 0);

  lv_obj_t *back = lv_button_create(g_config_panel);
  lv_obj_set_size(back, 64, 32);
  lv_obj_set_pos(back, 10, 8);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x26313A), 0);
  lv_obj_set_style_radius(back, 7, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_add_event_cb(back, back_to_launcher_cb, LV_EVENT_RELEASED, nullptr);
  lv_obj_t *back_label = lv_label_create(back);
  lv_label_set_text(back_label, ocean_ui_text("返回", "Back"));
  lv_obj_set_style_text_font(back_label, codex_font_16(), 0);
  lv_obj_set_style_text_color(back_label, lv_color_hex(0xDCE5EA), 0);
  lv_obj_center(back_label);

  create_ui_label(g_config_panel, ocean_ui_text("电子海洋参数", "Ocean settings"),
                  90, 9, 180, lv_color_hex(0xF7FAFC), true);
  create_ui_label(g_config_panel, ocean_ui_text("设置后点击进入", "Adjust, then enter"),
                  282, 13, 220, lv_color_hex(0x8A99A5));

  g_enter_button = lv_button_create(g_config_panel);
  lv_obj_set_size(g_enter_button, 92, 34);
  lv_obj_set_pos(g_enter_button, 538, 7);
  lv_obj_set_style_bg_color(g_enter_button, lv_color_hex(0x167D68), 0);
  lv_obj_set_style_radius(g_enter_button, 8, 0);
  lv_obj_set_style_border_width(g_enter_button, 0, 0);
  lv_obj_add_event_cb(g_enter_button, enter_ocean_cb, LV_EVENT_RELEASED, nullptr);
  lv_obj_t *enter_label = lv_label_create(g_enter_button);
  lv_label_set_text(enter_label, ocean_ui_text("进入", "Enter"));
  lv_obj_set_style_text_font(enter_label, codex_font_16(), 0);
  lv_obj_set_style_text_color(enter_label, lv_color_white(), 0);
  lv_obj_center(enter_label);

  lv_obj_t *idle_card = create_setting_card(10, 145);
  create_ui_label(idle_card, ocean_ui_text("静置时间", "Idle time"),
                  10, 9, 82, lv_color_hex(0xC6D1D8));
  g_idle_value_label = create_ui_label(idle_card, "", 84, 9, 51, lv_color_hex(0x74B9FF));
  lv_obj_set_style_text_align(g_idle_value_label, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_t *idle_slider = lv_slider_create(idle_card);
  lv_obj_set_size(idle_slider, 125, 24);
  lv_obj_set_pos(idle_slider, 10, 57);
  lv_slider_set_range(idle_slider, 1, 30);
  lv_slider_set_value(idle_slider, (int32_t)g_user_settings.idle_seconds, LV_ANIM_OFF);
  style_ocean_slider(idle_slider, lv_color_hex(0x74B9FF));
  lv_obj_add_event_cb(idle_slider, idle_time_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  refresh_idle_value_label();

  lv_obj_t *strength_card = create_setting_card(165, 145);
  create_ui_label(strength_card, ocean_ui_text("海浪强度", "Wave power"),
                  10, 9, 82, lv_color_hex(0xC6D1D8));
  g_strength_value_label = create_ui_label(strength_card, "", 92, 9, 43, lv_color_hex(0x78F0A4));
  lv_obj_set_style_text_align(g_strength_value_label, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_t *strength_slider = lv_slider_create(strength_card);
  lv_obj_set_size(strength_slider, 125, 24);
  lv_obj_set_pos(strength_slider, 10, 57);
  lv_slider_set_range(strength_slider, 1, 10);
  lv_slider_set_value(strength_slider, g_user_settings.wave_strength, LV_ANIM_OFF);
  style_ocean_slider(strength_slider, lv_color_hex(0x78F0A4));
  lv_obj_add_event_cb(strength_slider, wave_strength_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  refresh_strength_value_label();

  lv_obj_t *theme_card = create_setting_card(320, 155);
  create_ui_label(theme_card, ocean_ui_text("主题颜色", "Theme color"),
                  10, 9, 130, lv_color_hex(0xC6D1D8));
  for (int i = 0; i < OCEAN_THEME_COUNT; ++i) {
    lv_obj_t *button = lv_button_create(theme_card);
    g_theme_buttons[i] = button;
    lv_obj_set_size(button, 26, 26);
    lv_obj_set_pos(button, 10 + i * 34, 56);
    lv_obj_set_style_bg_color(button, lv_color_hex(OCEAN_THEME_PREVIEWS[i]), 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, theme_selected_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
  }
  refresh_theme_buttons();

  lv_obj_t *random_card = create_setting_card(485, 145);
  create_ui_label(random_card, ocean_ui_text("随机造浪", "Random waves"),
                  10, 9, 125, lv_color_hex(0xC6D1D8));
  lv_obj_t *random_switch = lv_switch_create(random_card);
  lv_obj_set_size(random_switch, 58, 30);
  lv_obj_set_pos(random_switch, 10, 54);
  lv_obj_set_style_bg_color(random_switch, lv_color_hex(0x2A3740), LV_PART_MAIN);
  lv_obj_set_style_bg_color(random_switch, lv_color_hex(0x33B58F), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(random_switch, lv_color_hex(0xF7FAFC), LV_PART_KNOB);
  if (g_user_settings.random_waves) lv_obj_add_state(random_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(random_switch, random_wave_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  g_random_value_label = create_ui_label(random_card, "", 78, 58, 55, lv_color_hex(0x78F0A4));
  lv_label_set_text(g_random_value_label,
                    g_user_settings.random_waves ? ocean_ui_text("开启", "On")
                                                 : ocean_ui_text("关闭", "Off"));
}

}  // namespace

static lv_obj_t *app_ocean_water_create_with_config(const OceanConfig *config)
{
  g_config = config;
  memset(&g_stats, 0, sizeof(g_stats));
  lvgl_port_set_crop(false, 0);
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);

  g_ocean_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_ocean_scr, lv_color_hex(0x101418), 0);
  lv_obj_set_style_bg_opa(g_ocean_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_ocean_scr, 0, 0);
  lv_obj_clear_flag(g_ocean_scr, LV_OBJ_FLAG_SCROLLABLE);

  build_config_ui();
  Serial.println("OCEAN config_opened");
  return g_ocean_scr;
}

lv_obj_t *app_ocean_water_create(void)
{
  return app_ocean_water_create_with_config(&OCEAN_CONFIG_STANDARD);
}

void app_ocean_water_destroy(lv_obj_t *scr)
{
  (void)scr;
  stop_simulation();

  if (g_config != nullptr && g_stats.ready && g_stats.frames > 0 && g_stats.elapsed_us > 0) {
    uint32_t fps_x10 = (uint32_t)(((uint64_t)g_stats.frames * 10000000ULL) / g_stats.elapsed_us);
    Serial.printf("OCEAN result mode=%s grid=%dx%d pressure=%d fps=%u.%u imu=%luus flip=%luus draw=%luus\n",
                  g_config->mode_name, g_config->grid_w, g_config->grid_h, g_config->pressure_iters,
                  fps_x10 / 10, fps_x10 % 10,
                  (unsigned long)(g_stats.imu_us / g_stats.frames),
                  (unsigned long)(g_stats.flip_us / g_stats.frames),
                  (unsigned long)(g_stats.draw_us / g_stats.frames));
  }

  if (g_task_done != nullptr) {
    vSemaphoreDelete(g_task_done);
    g_task_done = nullptr;
  }
  if (g_direct_mode_active) {
    lvgl_port_direct_mode_end();
    g_direct_mode_active = false;
  }

  lvgl_port_set_crop(false, 0);
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
  g_config_panel = nullptr;
  g_idle_value_label = nullptr;
  g_strength_value_label = nullptr;
  g_random_value_label = nullptr;
  g_enter_button = nullptr;
  memset(g_theme_buttons, 0, sizeof(g_theme_buttons));
  g_ocean_scr = nullptr;
  g_config = nullptr;
  Serial.println("OCEAN app_stopped");
}

void app_ocean_water_tick(lv_obj_t *scr)
{
  (void)scr;
}

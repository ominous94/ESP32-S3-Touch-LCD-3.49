#include "app_ocean_water.h"
#include "ocean_flip.h"
#include "i2c_bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

namespace {

constexpr int OCEAN_LEVEL_MAX = 20;
constexpr uint32_t OCEAN_FRAME_MS = 33;
constexpr float OCEAN_ACCEL_SCALE = 4.0f / 32768.0f;
constexpr float OCEAN_ACCEL_ALPHA = 0.30f;
constexpr uint32_t OCEAN_PROFILE_FRAMES = 120;

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

static const OceanConfig OCEAN_CONFIG_STANDARD = {
  "standard", 8, 32, 20, 19, 10,
};

constexpr uint8_t OCEAN_QMI_REG_WHOAMI = 0x00;
constexpr uint8_t OCEAN_QMI_REG_CTRL1 = 0x02;
constexpr uint8_t OCEAN_QMI_REG_CTRL2 = 0x03;
constexpr uint8_t OCEAN_QMI_REG_CTRL5 = 0x06;
constexpr uint8_t OCEAN_QMI_REG_CTRL7 = 0x08;
constexpr uint8_t OCEAN_QMI_REG_STATUS0 = 0x2E;
constexpr uint8_t OCEAN_QMI_REG_AX_L = 0x35;
constexpr uint8_t OCEAN_QMI_REG_RESET = 0x60;
constexpr uint8_t OCEAN_QMI_WHOAMI_VALUE = 0x05;

static lv_obj_t *g_ocean_scr = nullptr;
static TaskHandle_t g_sim_task = nullptr;
static SemaphoreHandle_t g_task_done = nullptr;
static volatile bool g_sim_running = false;
static bool g_direct_mode_active = false;
static const OceanConfig *g_config = nullptr;
static OceanStats g_stats = {};
static uint16_t g_palette[OCEAN_LEVEL_MAX + 1] = {};

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

  // 自动递增地址、小端；加速度量程 ±4g、ODR 1000Hz，并启用低通滤波。
  if (!imu_write(OCEAN_QMI_REG_CTRL1, 0x40)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL2, 0x13)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL5, 0x11)) return false;
  if (!imu_write(OCEAN_QMI_REG_CTRL7, 0x01)) return false;
  delay(50);
  return true;
}

static bool ocean_read_gravity(float *gx, float *gy)
{
  uint8_t status = 0;
  if (!imu_read(OCEAN_QMI_REG_STATUS0, &status, 1) || !(status & 0x01)) return false;

  uint8_t data[6];
  if (!imu_read(OCEAN_QMI_REG_AX_L, data, sizeof(data))) return false;
  int16_t raw_x = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
  int16_t raw_y = (int16_t)(((uint16_t)data[3] << 8) | data[2]);
  float ax = (float)raw_x * OCEAN_ACCEL_SCALE;
  float ay = (float)raw_y * OCEAN_ACCEL_SCALE;

  // QMI8658 轴到 rotation 0 屏幕坐标的映射。
  *gx = ay;
  *gy = ax;
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

static void build_palette()
{
  struct Stop {
    float position;
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  static const Stop stops[] = {
    {0.00f,   0,   0,   0},
    {0.12f,   0,   8,  28},
    {0.35f,   0,  35, 105},
    {0.62f,   0, 100, 185},
    {0.82f,  18, 185, 228},
    {1.00f, 170, 246, 255},
  };

  for (int level = 0; level <= OCEAN_LEVEL_MAX; ++level) {
    float t = (float)level / (float)OCEAN_LEVEL_MAX;
    const Stop *a = &stops[0];
    const Stop *b = &stops[1];
    for (size_t i = 1; i < sizeof(stops) / sizeof(stops[0]); ++i) {
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
  float filtered_gx = 0.0f;
  float filtered_gy = 1.0f;
  bool gravity_initialized = false;
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
      float gx = filtered_gx;
      float gy = filtered_gy;
      if (imu_ready) {
        float measured_gx = 0.0f;
        float measured_gy = 0.0f;
        if (ocean_read_gravity(&measured_gx, &measured_gy)) {
          if (!gravity_initialized) {
            filtered_gx = measured_gx;
            filtered_gy = measured_gy;
            gravity_initialized = true;
          } else {
            filtered_gx += (measured_gx - filtered_gx) * OCEAN_ACCEL_ALPHA;
            filtered_gy += (measured_gy - filtered_gy) * OCEAN_ACCEL_ALPHA;
          }
          gx = filtered_gx;
          gy = filtered_gy;
        }
      } else {
        const float seconds = (float)millis() * 0.001f;
        gx = sinf(seconds * 0.72f) * 0.34f;
        gy = 1.0f;
      }
      if (profile_this_frame) g_stats.imu_us += esp_timer_get_time() - stage_started_us;

      if (profile_this_frame) stage_started_us = esp_timer_get_time();
      ocean_flip_step(fluid, dt, gx, gy);
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

}  // namespace

static lv_obj_t *app_ocean_water_create_with_config(const OceanConfig *config)
{
  g_config = config;
  memset(&g_stats, 0, sizeof(g_stats));
  lvgl_port_set_crop(false, 0);
  lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_0);

  g_ocean_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_ocean_scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_ocean_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_ocean_scr, 0, 0);
  lv_obj_clear_flag(g_ocean_scr, LV_OBJ_FLAG_SCROLLABLE);

  g_direct_mode_active = lvgl_port_direct_mode_begin();
  if (!g_direct_mode_active) {
    Serial.println("OCEAN init_failed reason=direct_mode");
    lv_obj_t *label = lv_label_create(g_ocean_scr);
    lv_label_set_text(label, "Direct display init failed");
    lv_obj_center(label);
    return g_ocean_scr;
  }

  build_palette();
  g_task_done = xSemaphoreCreateBinary();
  if (g_task_done == nullptr) {
    Serial.println("OCEAN init_failed reason=semaphore");
    return g_ocean_scr;
  }

  g_sim_running = true;
  BaseType_t created = xTaskCreatePinnedToCore(ocean_sim_task, "ocean_sim", 8 * 1024,
                                               nullptr, 1, &g_sim_task, 1);
  if (created != pdPASS) {
    g_sim_running = false;
    g_sim_task = nullptr;
    Serial.println("OCEAN init_failed reason=task_create");
  } else {
    Serial.printf("OCEAN app_started renderer=direct_dma mode=%s grid=%dx%d pressure=%d\n",
                  config->mode_name, config->grid_w, config->grid_h, config->pressure_iters);
  }
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
  g_ocean_scr = nullptr;
  g_config = nullptr;
  Serial.println("OCEAN app_stopped");
}

void app_ocean_water_tick(lv_obj_t *scr)
{
  (void)scr;
}

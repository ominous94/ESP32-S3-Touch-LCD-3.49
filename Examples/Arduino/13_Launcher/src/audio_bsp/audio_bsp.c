#include "audio_bsp.h"
#include "../codec_board/codec_board.h"
#include "../codec_board/codec_init.h"
#include "../esp_codec_dev/include/esp_codec_dev.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "AUDIO_BSP";

static esp_codec_dev_handle_t s_playback = NULL;
static SemaphoreHandle_t s_play_sem  = NULL;
static TaskHandle_t        s_task     = NULL;
static volatile bool       s_busy     = false;
static int                 s_volume   = 60;
static bool                s_inited   = false;

/* ── 提示音波形（程序内生成，不需要外部文件）── */
#define TONE_SAMPLE_RATE 24000
/* "叮咚"：880Hz(120ms) + 静音(40ms) + 660Hz(180ms) */
#define N1    (TONE_SAMPLE_RATE * 120 / 1000)   /* 2880 */
#define NGAP  (TONE_SAMPLE_RATE *  40 / 1000)   /*  960 */
#define N2    (TONE_SAMPLE_RATE * 180 / 1000)   /* 4320 */
#define N_TOTAL (N1 + NGAP + N2)                /* 8160 */
static int16_t *s_tone_buf = NULL;   /* stereo interleaved L,R,L,R... */
static size_t   s_tone_bytes = N_TOTAL * 2 * 2;

static void generate_tone_buf(void)
{
    s_tone_buf = (int16_t *)heap_caps_malloc(s_tone_bytes, MALLOC_CAP_SPIRAM);
    if (s_tone_buf == NULL) {
        ESP_LOGE(TAG, "Failed to alloc tone buf (%u bytes)", (unsigned)s_tone_bytes);
        return;
    }

    const float pi2 = 2.0f * 3.14159265f;
    int idx = 0;

    /* Tone 1: 880 Hz, 120ms, fade in 8ms / fade out 25ms */
    int fade_in  = TONE_SAMPLE_RATE *  8 / 1000;
    int fade_out = TONE_SAMPLE_RATE * 25 / 1000;
    for (int i = 0; i < N1; i++) {
        float env = 1.0f;
        if (i < fade_in)       env = (float)i / fade_in;
        else if (i > N1 - fade_out) env = (float)(N1 - i) / fade_out;
        if (env < 0) env = 0; if (env > 1) env = 1;
        int16_t v = (int16_t)(sinf(pi2 * 880.0f * i / TONE_SAMPLE_RATE) * 20000.0f * env);
        s_tone_buf[idx++] = v;   /* L */
        s_tone_buf[idx++] = v;   /* R */
    }

    /* Gap: 40ms silence */
    for (int i = 0; i < NGAP; i++) {
        s_tone_buf[idx++] = 0;
        s_tone_buf[idx++] = 0;
    }

    /* Tone 2: 660 Hz, 180ms, fade in 8ms / fade out 40ms */
    fade_in  = TONE_SAMPLE_RATE *  8 / 1000;
    fade_out = TONE_SAMPLE_RATE * 40 / 1000;
    for (int i = 0; i < N2; i++) {
        float env = 1.0f;
        if (i < fade_in)       env = (float)i / fade_in;
        else if (i > N2 - fade_out) env = (float)(N2 - i) / fade_out;
        if (env < 0) env = 0; if (env > 1) env = 1;
        int16_t v = (int16_t)(sinf(pi2 * 660.0f * i / TONE_SAMPLE_RATE) * 18000.0f * env);
        s_tone_buf[idx++] = v;   /* L */
        s_tone_buf[idx++] = v;   /* R */
    }

    ESP_LOGI(TAG, "Tone buffer generated: %u samples, %u bytes",
             (unsigned)N_TOTAL, (unsigned)s_tone_bytes);
}

/* ── 后台播放任务 ── */
static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_play_sem, portMAX_DELAY) != pdTRUE) continue;
        if (s_playback == NULL || s_tone_buf == NULL) continue;

        s_busy = true;
        esp_codec_dev_set_out_vol(s_playback, (float)s_volume);
        int ret = esp_codec_dev_write(s_playback, s_tone_buf, s_tone_bytes);
        if (ret != 0) {
            ESP_LOGW(TAG, "codec write ret=%d", ret);
        }
        s_busy = false;
    }
}

/* ── 公开 API ── */

int audio_bsp_init(void)
{
    if (s_inited) return 0;

    ESP_LOGI(TAG, "Initializing codec...");

    /* 设置板型 → 解析 board_cfg.h 中的 S3_LCD_3_49 配置 */
    set_codec_board_type("S3_LCD_3_49");

    /* 仅需输出（播放），不需要输入（录音）
     * 使用 STD 模式即可驱动 ES8311 DAC */
    codec_init_cfg_t codec_cfg = {
        .in_mode      = CODEC_I2S_MODE_NONE,
        .out_mode     = CODEC_I2S_MODE_STD,
        .in_use_tdm   = false,
        .reuse_dev    = false,
    };
    int ret = init_codec(&codec_cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "init_codec failed: %d", ret);
        return -1;
    }

    s_playback = get_playback_handle();
    if (s_playback == NULL) {
        ESP_LOGE(TAG, "playback handle is NULL");
        return -1;
    }

    /* 打开播放通道 */
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = TONE_SAMPLE_RATE;
    fs.channel         = 2;
    fs.bits_per_sample = 16;
    ret = esp_codec_dev_open(s_playback, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "codec open failed: %d", ret);
        s_playback = NULL;
        return -1;
    }

    esp_codec_dev_set_out_vol(s_playback, (float)s_volume);

    /* 生成提示音波形 */
    generate_tone_buf();

    /* 创建信号量 + 后台任务 */
    s_play_sem = xSemaphoreCreateBinary();
    if (s_play_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return -1;
    }

    xTaskCreatePinnedToCore(audio_task, "audio_bsp", 6 * 1024, NULL, 3, &s_task, 1);

    s_inited = true;
    ESP_LOGI(TAG, "Codec initialized OK (vol=%d)", s_volume);
    return 0;
}

void audio_bsp_play_complete_sound(void)
{
    if (!s_inited || s_play_sem == NULL) return;
    if (s_busy) return;  /* 上次还在播放，忽略 */
    xSemaphoreGive(s_play_sem);
}

void audio_bsp_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_playback != NULL) {
        esp_codec_dev_set_out_vol(s_playback, (float)vol);
    }
}

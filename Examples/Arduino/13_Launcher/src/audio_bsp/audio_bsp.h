#ifndef AUDIO_BSP_H
#define AUDIO_BSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 ES8311 音频 codec（I2S + I2C + PA 使能）
 * 必须在 i2c_master_Init() 和 tca9554_init() 之后调用
 * @return 0 成功，非 0 失败
 */
int audio_bsp_init(void);

/**
 * 播放完成提示音（短促的双音 "叮咚"）
 * 在后台 task 中执行，不阻塞调用方
 * 如果上次提示音还在播放，会忽略新请求
 */
void audio_bsp_play_complete_sound(void);

/**
 * 设置音量 (0-100)
 */
void audio_bsp_set_volume(int vol);

#ifdef __cplusplus
}
#endif

#endif

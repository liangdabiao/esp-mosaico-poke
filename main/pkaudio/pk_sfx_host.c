// main/wq_sfx_host.c —— 宿主模拟器的音效桩。
//
// 真机用 ES8311 I2S 合成（main/wq/wq_sfx.c + rtttl_player.c，依赖 bsp/audio.h 与
// esp_codec_dev.h，host 没有这些）；host 模拟器无音频设备，这里把同一份接口
// 实现为空操作 —— 调用方（game.c）在两端代码一致，host 下自然静音。
// 该文件只出现在 game.sim.json（host），不进设备 CMakeLists。
#include "wq_sfx.h"

esp_err_t wq_sfx_start(void)
{
    return ESP_OK; /* host：无喇叭，标记为「静音就绪」，不影响玩法。 */
}

void wq_sfx_stop(void)
{
    /* no-op */
}

void wq_sfx_play(wq_tone_t tone)
{
    (void)tone; /* host：无音频输出，丢弃。device-only。 */
}

void wq_sfx_set_volume(uint8_t level)
{
    (void)level; /* host：无 codec，记录在 game 层即可。 */
}

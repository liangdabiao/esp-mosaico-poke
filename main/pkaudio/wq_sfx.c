// main/wq_sfx.c —— 见 wq_sfx.h。
#include "wq_sfx.h"

#include <stddef.h>

#include "bsp/audio.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#include "rtttl_player.h"
#include "wq_volume.h"

// 音效旋律。RTTTL 格式：名字:默认(d=时长,o=八度,b=速度):音符序列。
// 设计取向：短、单音、黑白键感 —— 落子是低音「嗒」，提子高一点，
// 对/错用一个上行/下行小二度表达情绪，章节通关给一段明亮琶音。
static const char *const TONES[WQ_TONE_COUNT] = {
    // 移动：极短的无声「嗒」其实听不见，用最低八度的短音
    [WQ_TONE_MOVE] = "mv:d=4,o=5,b=500:c6",
    // 落子：低音短促，像棋子磕在木盘上
    [WQ_TONE_PLACE] = "pl:d=16,o=4,b=500:g",
    // 提子：两个上行音
    [WQ_TONE_CAPTURE] = "cp:d=8,o=5,b=300:c,g",
    // 确认
    [WQ_TONE_ENTER] = "en:d=8,o=5,b=300:e",
    // 返回：下行
    [WQ_TONE_BACK] = "bk:d=8,o=5,b=300:g,e",
    // 过关：上行三度
    [WQ_TONE_CORRECT] = "ok:d=8,o=5,b=280:c,e,g",
    // 走错：下行小二度
    [WQ_TONE_WRONG] = "no:d=4,o=4,b=250:f,e",
    // 章节通关：明亮琶音
    [WQ_TONE_COMPLETE] = "wn:d=8,o=5,b=200:c,e,g,c6",
};

// RTTTL 播放器出厂的 codec 音量（rtttl_player.c 的 RTTTL_VOLUME）。
// 音量档位以它为 100% 基准缩放，默认档 80% 时比原始 RTTTL 略轻，
// 但各档位之间单调、可复现，且与侨批的「基准值在档位下整除」同构。
#define WQ_SFX_BASE_VOLUME 58

static uint8_t s_level = WQ_VOLUME_DEFAULT;
static esp_codec_dev_handle_t s_speaker;

void wq_sfx_set_volume(uint8_t level)
{
    s_level = level;
    if (!s_speaker) s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker) {
        (void)esp_codec_dev_set_out_vol(s_speaker,
                                        wq_volume_scale(WQ_SFX_BASE_VOLUME, level));
    }
}

esp_err_t wq_sfx_start(void)
{
    return rtttl_player_start();
}

void wq_sfx_stop(void)
{
    rtttl_player_stop();
}

void wq_sfx_play(wq_tone_t tone)
{
    if (tone >= WQ_TONE_COUNT) return;
    if (s_level == WQ_VOLUME_OFF) return;
    (void)rtttl_player_play(TONES[tone]);
}

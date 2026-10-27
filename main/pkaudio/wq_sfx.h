// main/wq_sfx.h —— 界面音效：RTTTL 合成，不依赖任何音频素材。
//
// 本应用没有 BGM 也没有配音（用户拍板：2.78MB 背景音乐不要），所以 I2S 出口
// 只有 rtttl_player 一个写入者 —— 不存在侨批「两个写入者交错写 DMA」的问题，
// 不需要 qpq_player 那样的统一播放服务。音量档位沿用 wq_volume 的档位表，
// 通过 codec 音量缩放。
#pragma once

#include <stdint.h>

#include "esp_err.h"

// 音效名。与网页版的 click / correct / wrong / complete 对应，另加返回与移动，
// 让三个键在菜单里有区分。
typedef enum {
    WQ_TONE_MOVE = 0,   // 光标移动
    WQ_TONE_PLACE,      // 落子（棋子拍在棋盘上）
    WQ_TONE_CAPTURE,    // 提子
    WQ_TONE_ENTER,      // 确认、进入
    WQ_TONE_BACK,       // 返回
    WQ_TONE_CORRECT,    // 过关
    WQ_TONE_WRONG,      // 走错
    WQ_TONE_COMPLETE,   // 章节通关
    WQ_TONE_COUNT,
} wq_tone_t;

// 启动 rtttl_player 任务。codec 不可用时返回错误，应用继续以静音方式运行。
esp_err_t wq_sfx_start(void);

// 停止任务（生命周期用）。调用后可以再次 start。
void wq_sfx_stop(void);

// 播放一道音效。音量为关（0 档）时静默丢弃。
void wq_sfx_play(wq_tone_t tone);

// 应用音量档位（0 = 关）。开机与切档时调用；内部换算成 codec 音量。
void wq_sfx_set_volume(uint8_t level);

// main/wq_volume.c —— 见 wq_volume.h。
#include "wq_volume.h"

#include <stdio.h>
#include <string.h>

const uint8_t wq_volume_steps[WQ_VOLUME_STEP_COUNT] = {
    WQ_VOLUME_OFF, 20, 40, 60, WQ_VOLUME_DEFAULT, WQ_VOLUME_MAX,
};

// 默认档位必须在档位表里，否则「出厂默认」会是一个用户永远调不回来的值。
_Static_assert(WQ_VOLUME_DEFAULT % 20 == 0, "默认档位必须落在 20% 的格点上");
_Static_assert(WQ_VOLUME_DEFAULT < WQ_VOLUME_MAX, "默认档位不能是满量程，否则用户没法再调大");
_Static_assert((int)WQ_VOLUME_DEFAULT / 20 < WQ_VOLUME_STEP_COUNT, "默认档位越出档位表");
_Static_assert(WQ_VOLUME_MAX == 100, "缩放按百分比算，最大值必须是 100");

uint8_t wq_volume_scale(uint8_t base, uint8_t level)
{
    // 先转 32 位再乘：base 最大 100、level 最大 255 时 8 位乘法会溢出。
    // 截断而不是四舍五入：与侨批同一套算术，行为可复现。
    return (uint8_t)(((uint32_t)base * (uint32_t)level) / 100u);
}

uint8_t wq_volume_next(uint8_t level)
{
    for (int index = 0; index < WQ_VOLUME_STEP_COUNT; index++) {
        if (wq_volume_steps[index] == level) {
            return wq_volume_steps[(index + 1) % WQ_VOLUME_STEP_COUNT];
        }
    }
    // 不认识的档位：回到关。返回一个「表里没有的值」会让下一次调用也找不到，
    // 于是用户再也调不动音量 —— 那才是真的坏掉。
    return wq_volume_steps[0];
}

uint16_t wq_volume_text(uint8_t level, char *out, size_t capacity)
{
    if (!out || capacity == 0) return 0;
    out[0] = '\0';

    if (level == WQ_VOLUME_OFF) {
        // 「关」是一个汉字，UTF-8 占 3 字节。容量不足时整体失败，不留半截。
        static const char k_off[] = "关";
        if (sizeof(k_off) > capacity) return 0;
        memcpy(out, k_off, sizeof(k_off));
        return (uint16_t)(sizeof(k_off) - 1u);
    }

    char buffer[8];
    const int written = snprintf(buffer, sizeof(buffer), "%u%%", (unsigned)level);
    if (written <= 0 || (size_t)written + 1u > capacity) return 0;
    memcpy(out, buffer, (size_t)written + 1u);
    return (uint16_t)written;
}

// main/capture.c —— S2 捕获判定（时机窗口 + 投球）。
//
// PC 侧是 sim/systems.py 的 window_width / pointer_position /
// attempt_capture。**这一整块是确定性的**，所以与 sim 逐值对账
// （tools/pipeline/verify_battle.py），不像战斗那样只能对分布。
//
// ## 为什么是时机判定而不是概率掷骰
//
// 概率掷骰玩家只能接受结果；时机判定让玩家**参与**。
// 这是整个游戏唯一需要手眼配合的地方，也是「养成反哺探索」的落点：
// 心情高 → 窗口宽（P4 页面文档记的四个乘数四条能动性）。
//
// 命中判定因此是**确定性的** —— 给定按键时刻与状态，结果唯一。
// 只有「失败后是否逃跑」用随机，因为那不该被玩家预测。

#include <string.h>

#include "capture.h"

// crc32 —— 逃跑判定用确定性种子。原版 host/device 各挂一份 zlib / ROM 实现；
// 本项目统一走 pk_crc32（标准 CRC-32/IEEE，两端同值，见 pk_port.h）。
#include "pk_port.h"
#define CRC32(buf, len) pk_crc32((buf), (len))

// 基础窗口的压缩系数 ×1000。
//
// **不能省这一步**：capture_rate 直接当像素会让高捕获率的种类
// （皮卡丘 190）一上来就撞满 200px 的上界，于是球种与养成加成
// 全部失效 —— 乘数再大也顶不动上界。实测发现的（190×任何系数都是 200）。
//
// 压到 0.55 后皮卡丘基础 104px：精灵球刚过半条、高级球才接近满条，
// 球种选择重新变成有意义的决策。
#define BASE_SCALE_1000 550

// 球种系数 ×1000。索引 = cap_ball_t
uint16_t cap_ball_factor_1000(cap_ball_t ball, const cap_context_t *context)
{
    if (ball == CAP_BALL_GREAT) return 1500;
    if (ball == CAP_BALL_ULTRA) return 2000;
    if (!context) return 1000;
    if (ball == CAP_BALL_FAST && context->wild_speed >= 100) return 4000;
    if (ball == CAP_BALL_HEAVY) {
        if (context->wild_weight_hg >= 2000) return 3000;
        if (context->wild_weight_hg >= 1000) return 2000;
    }
    if (ball == CAP_BALL_LEVEL && context->wild_level) {
        if (context->pet_level >= 2u * context->wild_level) return 2000;
        if (context->pet_level > context->wild_level) return 1500;
    }
    return 1000;
}

const char *cap_ball_name(cap_ball_t b)
{
    static const char *N[CAP_BALL_COUNT] = {"精灵球", "超级球", "高级球", "大师球",
        "速度球", "沉重球", "等级球", "友友球"};
    return ((unsigned)b < CAP_BALL_COUNT) ? N[b] : "精灵球";
}

// 逃跑概率 ×1000，按稀有度。⏳ 这条曲线纯凭手感，待实测校准
// （sim/systems.py 的 FLEE_CHANCE 上也挂着同一个待办）。
static const uint16_t FLEE_1000[6] = {200, 100, 180, 280, 380, 500};

uint16_t cap_window_width(uint8_t capture_rate, uint16_t mood_bonus_q10,
                          cap_ball_t ball, uint8_t hp_ratio)
{
    return cap_window_width_context(capture_rate, mood_bonus_q10, ball, hp_ratio, NULL);
}

uint16_t cap_window_width_context(uint8_t capture_rate, uint16_t mood_bonus_q10,
                                  cap_ball_t ball, uint8_t hp_ratio,
                                  const cap_context_t *context)
{
    // 四个乘数，对应四条不同来源的玩家能动性：
    //   capture_rate 种族固有（改不了）· mood 养成 · ball 探索 · hp 战斗
    //
    // **先连乘再一次性除**，不要每步都除。
    //
    // 第一版每步除，对账红了 75 组：capture_rate=3 时第一步
    // `3 * 550 / 1000` 把 1.65 截成 1，后面的乘数再大也只能从 1 起算，
    // 高级球 + 濒死本该给到 7px，实测只有 3px（撞在下界上）。
    // 稀有种（capture_rate 个位数）正是最需要那几个乘数生效的地方，
    // 而截断恰好把它们废掉了 —— 越稀有越失真。
    //
    // 溢出边界：255 × 550 × 2048 × 2000 × 199 ≈ 1.14e14，
    // 超 u32 但 u64 绰绰有余（u64 上限 1.8e19）。
    if (hp_ratio > 100) hp_ratio = 100;
    if (ball == CAP_BALL_MASTER) return CAP_WINDOW_MAX;
    uint16_t bf = cap_ball_factor_1000(ball, context);

    uint64_t num = (uint64_t)capture_rate * BASE_SCALE_1000
                 * mood_bonus_q10 * bf * (uint32_t)(100 + (100 - hp_ratio));
    uint64_t den = 1000ULL * 1024 * 1000 * 100;

    // 四舍五入 —— Python 那边是 int(round(w))，截断会系统性偏低 1px
    uint32_t w = (uint32_t)((num + den / 2) / den);

    if (w < CAP_WINDOW_MIN) w = CAP_WINDOW_MIN;
    if (w > CAP_WINDOW_MAX) w = CAP_WINDOW_MAX;
    return (uint16_t)w;
}

uint16_t cap_pointer_position(uint32_t elapsed_ms)
{
    // 三角波往复。不用三角函数 —— 整数运算即可，
    // 而且 C3 无 FPU，sinf 一次几百周期而这个每帧都要算。
    uint32_t phase = elapsed_ms % CAP_POINTER_PERIOD_MS;
    uint32_t half = CAP_POINTER_PERIOD_MS / 2;
    if (phase < half) {
        return (uint16_t)(phase * CAP_BAR_WIDTH / half);
    }
    return (uint16_t)(CAP_BAR_WIDTH -
                      (phase - half) * CAP_BAR_WIDTH /
                      (CAP_POINTER_PERIOD_MS - half));
}

void cap_attempt(uint8_t capture_rate, uint16_t mood_bonus_q10,
                 cap_ball_t ball, uint8_t hp_ratio, uint8_t rarity,
                 uint32_t elapsed_ms, uint32_t seed,
                 cap_result_t *out)
{
    cap_attempt_context(capture_rate, mood_bonus_q10, ball, hp_ratio, rarity,
                         elapsed_ms, seed, NULL, out);
}

void cap_attempt_context(uint8_t capture_rate, uint16_t mood_bonus_q10,
                         cap_ball_t ball, uint8_t hp_ratio, uint8_t rarity,
                         uint32_t elapsed_ms, uint32_t seed,
                         const cap_context_t *context, cap_result_t *out)
{
    memset(out, 0, sizeof(*out));

    uint16_t w = cap_window_width_context(capture_rate, mood_bonus_q10, ball, hp_ratio, context);
    // 窗口居中
    uint16_t start = (uint16_t)((CAP_BAR_WIDTH - w) / 2);
    uint16_t p = cap_pointer_position(elapsed_ms);

    out->window_w = w;
    out->window_start = start;
    out->window_end = (uint16_t)(start + w);
    out->pointer = p;
    out->ball = ball;
    out->caught = (p >= start && p <= out->window_end);

    if (out->caught) return;

    // 逃跑判定 —— 用确定性种子（便于回放复现）但玩家无法预测。
    //
    // **与 PC 侧逐位一致**：那边是
    //     (zlib.crc32(str(seed).encode()) & 0xFFFF) / 65535.0 < chance
    // 这里把浮点比较改成整数交叉相乘，避免除法丢精度：
    //     (crc & 0xFFFF) * 1000 < chance_1000 * 65535
    char buf[12];
    uint32_t v = seed;
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[12];
        int t = 0;
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        while (t) buf[n++] = tmp[--t];
    }
    uint32_t crc = CRC32(buf, (size_t)n);
    uint32_t chance = (rarity <= 5) ? FLEE_1000[rarity] : 200;
    out->fled = ((uint64_t)(crc & 0xFFFF) * 1000 < (uint64_t)chance * 65535);
}

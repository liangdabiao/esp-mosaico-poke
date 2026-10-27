// main/nurture.h —— S4 养成状态机（三条轴 + 亲密度）。
//
// PC 侧对应 sim/gameplay.py 的 PetState。**衰减常量必须与那边一致**，
// 差一点就是不同的游戏 —— 那些数字是 docs/04-gameplay.md 标定过的。
//
// ## 定点表示
//
// 三条轴与亲密度在 PC 侧是 0~100 的 float。这里用 **Q10 定点**
// （值 × 1024），与 sensing.c 同一套约定：
//   · ESP32-C3 没有 FPU，浮点是软件模拟，一次乘法上百周期
//   · 衰减是每分钟算一次的常驻逻辑，不能是慢路径
//   · Q10 下 1 单位 = 0.001，而轴的显示精度是整数百分比，绰绰有余
//
// ## 时间来源
//
// `esp_timer_get_time()` 的开机微秒数。**它会漂** —— 实测配置是
// CONFIG_RTC_CLK_SRC_INT_RC（内部 RC 振荡器，不是外置晶振），
// 典型精度 ±5% 且有温漂。长跑实测见 tools/device/soak.py。
//
// 漂移对养成的影响是「衰减快慢略有偏差」，玩家感知不到；
// 但它影响 S10 的日切判定 —— 那条要等 soak 的数据出来再定。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NURT_Q       1024              // 定点标度（Q10），与 sensing.h 同
#define NURT_MAX     (100 * NURT_Q)    // 轴的上限 = 100.0

// 每小时衰减/恢复速率，Q10。与 sim/gameplay.py 的常量逐个对应：
//   SATIETY_DECAY_PER_HOUR   = 4.0
//   MOOD_DECAY_PER_HOUR      = 3.0
//   STAMINA_RECOVER_PER_HOUR = 100.0
#define NURT_SATIETY_DECAY_PH  (4 * NURT_Q)
#define NURT_MOOD_DECAY_PH     (3 * NURT_Q)
#define NURT_STAMINA_RECOVER_PH (100 * NURT_Q)
#define NURT_STAMINA_COST_PER_MOTION 0

// 亲密度：陪伴时长累积，0.5/小时（sim 侧 intimacy += hours * 0.5）
#define NURT_INTIMACY_PH (NURT_Q / 2)

// 饱食或心情低于此值进入「消沉」；体能不影响能力。
#define NURT_LOW_THRESHOLD (25 * NURT_Q)

// 互动增量（sim 侧 feed(30) / play()）
#define NURT_FEED_SATIETY  (30 * NURT_Q)
#define NURT_FEED_MOOD     (5 * NURT_Q)
#define NURT_PLAY_MOOD     (15 * NURT_Q)
#define NURT_PLAY_STAMINA  (5 * NURT_Q)
#define NURT_PLAY_INTIMACY (1 * NURT_Q)
#define NURT_REST_STAMINA  0
#define NURT_DEFEAT_STAMINA (20 * NURT_Q)
#define NURT_DEFEAT_MOOD    (15 * NURT_Q)
#define NURT_CHALLENGE_DEFEAT_MOOD (5 * NURT_Q)

typedef struct {
    int32_t satiety;      // Q10，0~NURT_MAX
    int32_t mood;
    int32_t stamina;
    int32_t intimacy;

    // 上次结算的时刻（微秒）。-1 = 还没结算过，首次 tick 只记录不衰减
    // —— 与 sim 侧 `if self._last_ts is None` 的行为一致。
    int64_t last_us;
} nurture_t;

// 心情档位。与 sim/strings.py 的 mood 文案一一对应。
typedef enum {
    NURT_MOOD_HAPPY = 0,   // 愉快
    NURT_MOOD_CALM,        // 平静
    NURT_MOOD_LOW,         // 低落
    NURT_MOOD_DESPONDENT,  // 消沉
} nurt_mood_t;

void nurture_init(nurture_t *n);

// 按时间推进。motion_events 是这段时间内的移动事件数（S1 给），
// is_night 决定体能恢复是否翻倍。
//
// 幂等：同一个 now_us 调两次，第二次不产生变化（hours <= 0 直接返回）。
void nurture_tick(nurture_t *n, int64_t now_us, int motion_events,
                  bool is_night);

void nurture_feed(nurture_t *n);
// Returns false without changing any axis when the full stamina cost is unavailable.
bool nurture_play(nurture_t *n);
void nurture_rest(nurture_t *n);
void nurture_defeat(nurture_t *n);
// Paid trainer battles keep their entry cost; defeat only affects mood.
void nurture_challenge_defeat(nurture_t *n);
// Cost is in whole stamina points. Use the exact Q10 balance, not its display rounding.
unsigned nurture_wait_minutes(const nurture_t *n, unsigned cost);
// Spendable whole points; a 4.9 balance must not be advertised as enough for 5.
uint8_t nurture_stamina_points(const nurture_t *n);
uint16_t nurture_ability_factor(const nurture_t *n);

// 三条轴取整成 0~100 —— 上屏用。
uint8_t nurture_pct(int32_t q);

nurt_mood_t nurture_mood(const nurture_t *n);

// 与 PC 侧逐项对账。宿主上编译运行，见 tools/pipeline/verify_nurture.py。
bool nurture_selftest(void);

#define NURT_EXPLORE_COST (5 * NURT_Q)
uint8_t nurture_exp_percent(const nurture_t *n);
uint8_t nurture_rare_bonus(const nurture_t *n);

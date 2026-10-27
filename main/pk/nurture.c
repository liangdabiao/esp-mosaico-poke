// main/nurture.c —— S4 养成状态机。
//
// PC 侧是 sim/gameplay.py 的 PetState。这份移植的验收标准是
// **逐拍对账**：同样的时间序列喂进去，两边的三条轴每一步都相等
// （见 tools/pipeline/verify_nurture.py）。
//
// ## 为什么衰减要按「经过的时长」算，而不是每 tick 减固定值
//
// 每 tick 减固定值的写法要求 tick 频率恒定。而这里的 tick 来自
// LVGL 定时器，会被 WiFi 扫描、SPI 推屏、按键处理挤掉 ——
// 实测扫描时一次 tick 能拖到 900ms。按固定值减，设备越忙宠物饿得越慢。
//
// 按时长算就没这个问题：漏了几拍，下一拍把欠的时间一次补上。
// 这也让「关机一晚再开机」自然成立（若接了 RTC 保持时间）。
//
// ## 定点乘法的溢出边界
//
// `rate * us / 3600e6` 若先乘后除会溢出：4096 × 3.6e9 远超 int32。
// 这里先把微秒折成 Q10 小时再乘，中间量用 int64 兜住。
// 最大值估算：rate 6144(Q10) × hours_q10 (一天 = 24576) = 1.5e8，
// int64 绰绰有余，结果右移 10 位回到 Q10。

#include <inttypes.h>
#include <stdio.h>

#include "nurture.h"

// 一小时的微秒数。写成 int64 常量 —— 写成 int 会在
// `now - last` 超过 2147 秒时溢出。
#define US_PER_HOUR 3600000000LL

static int32_t clamp_axis(int64_t v)
{
    if (v < 0) return 0;
    if (v > NURT_MAX) return NURT_MAX;
    return (int32_t)v;
}

void nurture_init(nurture_t *n)
{
    // 初值逐个抄自 sim/gameplay.py 的 PetState 字段默认值。
    // **不要凭印象写** —— 第一版我写成 70/70/80，与 PC 侧
    // 80/70/90 差了两条轴，对账脚本从第 0 拍就红。
    n->satiety = 80 * NURT_Q;
    n->mood = 70 * NURT_Q;
    n->stamina = 90 * NURT_Q;
    n->intimacy = 0;
    n->last_us = -1;              // 首次 tick 只记录，不衰减
}

void nurture_tick(nurture_t *n, int64_t now_us, int motion_events,
                  bool is_night)
{
    if (n->last_us < 0) {         // 与 sim 侧 `_last_ts is None` 对应
        n->last_us = now_us;
        return;
    }

    int64_t dt = now_us - n->last_us;
    if (dt <= 0) return;          // 幂等：同一时刻调两次，第二次无变化

    // 经过的小时数，Q10。
    int64_t hours_q = (dt * NURT_Q) / US_PER_HOUR;

    // **只推进已经结算掉的那部分时间**，余数留到下一拍。
    //
    // 这是定点时间累加最容易错的地方，而且错得很隐蔽：
    // 直接 `last_us = now_us` 会把不足一个量子的零头丢掉。
    // 一个 Q10 量子是 3.52 秒，而真机的 tick 间隔 250ms~8s ——
    // 实测相位②的 6 秒间隔每拍丢 41% 的时间，
    // 跑一小时后体能比 PC 侧低 1.5，屏幕上就是差一格。
    //
    // 第一版我只在 hours_q == 0 时保留余数，以为「够一个量子就没问题」，
    // 对账脚本照样红 —— 因为 hours_q == 1 时同样丢了 0.7 个量子。
    // 现在无论哪种情况都只吃掉 hours_q 对应的整量子时间。
    n->last_us += hours_q * US_PER_HOUR / NURT_Q;

    if (hours_q == 0 && motion_events == 0) return;   // 还没攒够，且没有移动

    n->satiety = clamp_axis(n->satiety -
                            (int64_t)NURT_SATIETY_DECAY_PH * hours_q / NURT_Q);
    n->mood = clamp_axis(n->mood -
                         (int64_t)NURT_MOOD_DECAY_PH * hours_q / NURT_Q);

    // 体能：夜间恢复翻倍（与作息挂钩），移动消耗
    int64_t recover = (int64_t)NURT_STAMINA_RECOVER_PH * hours_q / NURT_Q;
    if (is_night) recover *= 2;
    int64_t cost = (int64_t)NURT_STAMINA_COST_PER_MOTION * motion_events;
    n->stamina = clamp_axis(n->stamina + recover - cost);

    // 陪伴时长累积成亲密度
    n->intimacy = clamp_axis(n->intimacy +
                             (int64_t)NURT_INTIMACY_PH * hours_q / NURT_Q);
}

void nurture_feed(nurture_t *n)
{
    n->satiety = clamp_axis((int64_t)n->satiety + NURT_FEED_SATIETY);
    n->mood = clamp_axis((int64_t)n->mood + NURT_FEED_MOOD);
}

bool nurture_play(nurture_t *n)
{
    if (!n || n->stamina < NURT_PLAY_STAMINA) return false;
    n->mood = clamp_axis((int64_t)n->mood + NURT_PLAY_MOOD);
    n->stamina = clamp_axis((int64_t)n->stamina - NURT_PLAY_STAMINA);
    n->intimacy = clamp_axis((int64_t)n->intimacy + NURT_PLAY_INTIMACY);
    return true;
}

void nurture_rest(nurture_t *n)
{
    n->stamina = clamp_axis((int64_t)n->stamina + NURT_REST_STAMINA);
}

void nurture_defeat(nurture_t *n)
{
    if (!n) return;
    n->stamina = clamp_axis((int64_t)n->stamina - NURT_DEFEAT_STAMINA);
    n->mood = clamp_axis((int64_t)n->mood - NURT_DEFEAT_MOOD);
}

uint16_t nurture_ability_factor(const nurture_t *n)
{
    return n && nurture_mood(n) == NURT_MOOD_DESPONDENT ? 614 : 1024;
}

void nurture_challenge_defeat(nurture_t *n)
{
    if (n) n->mood = clamp_axis((int64_t)n->mood - NURT_CHALLENGE_DEFEAT_MOOD);
}

unsigned nurture_wait_minutes(const nurture_t *n, unsigned cost)
{
    if (!n || !cost) return 0;
    int64_t missing = (int64_t)cost * NURT_Q - n->stamina;
    if (missing <= 0) return 0;
    return (unsigned)((missing * 60 + NURT_STAMINA_RECOVER_PH - 1) / NURT_STAMINA_RECOVER_PH);
}

uint8_t nurture_stamina_points(const nurture_t *n)
{
    if (!n || n->stamina <= 0) return 0;
    unsigned points = n->stamina / NURT_Q;
    return points > 100 ? 100 : points;
}

uint8_t nurture_pct(int32_t q)
{
    // 四舍五入而非截断 —— 截断会让 99.9 显示成 99，
    // 而玩家刚喂完看到 99 会以为没生效。
    int v = (q + NURT_Q / 2) / NURT_Q;
    return (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
}

nurt_mood_t nurture_mood(const nurture_t *n)
{
    // 体能是行动预算，不参与战斗能力；饱食与心情继续关联养成。
    int32_t lo = n->satiety;
    if (n->mood < lo) lo = n->mood;
    if (lo < NURT_LOW_THRESHOLD) return NURT_MOOD_DESPONDENT;

    if (n->mood >= 80 * NURT_Q) return NURT_MOOD_HAPPY;
    if (n->mood >= 50 * NURT_Q) return NURT_MOOD_CALM;
    return NURT_MOOD_LOW;
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------

bool nurture_selftest(void)
{
    bool ok = true;
    nurture_t n;

    // ① 首次 tick 不衰减（对应 sim 的 _last_ts is None）
    nurture_init(&n);
    nurture_tick(&n, 1000000, 0, false);
    if (n.satiety != 80 * NURT_Q) {
        printf("nurture: 首拍不该衰减，得到 %" PRId32 "\n", n.satiety);
        ok = false;
    }

    // ② 整一小时：饱食 70 → 66，心情 70 → 67，体能 80 → 86
    nurture_init(&n);
    nurture_tick(&n, 0, 0, false);
    nurture_tick(&n, US_PER_HOUR, 0, false);
    if (nurture_pct(n.satiety) != 76 || nurture_pct(n.mood) != 67 ||
        nurture_pct(n.stamina) != 100) {
        printf("nurture: 一小时后 %u/%u/%u，预期 76/67/100\n",
               nurture_pct(n.satiety), nurture_pct(n.mood),
               nurture_pct(n.stamina));
        ok = false;
    }

    // ③ 幂等：同一时刻重复调用不产生变化
    int32_t s = n.satiety;
    nurture_tick(&n, US_PER_HOUR, 0, false);
    if (n.satiety != s) {
        printf("nurture: 同一时刻重复 tick 改了值\n");
        ok = false;
    }

    // ④ **高频小步进等于一次大步进** —— 这条是定点实现最容易错的地方。
    // 250ms 的 tick 调 3600 次（共 15 分钟）应当与直接跳 15 分钟一致。
    nurture_t a, b;
    nurture_init(&a);
    nurture_init(&b);
    nurture_tick(&a, 0, 0, false);
    nurture_tick(&b, 0, 0, false);
    for (int i = 1; i <= 3600; i++) nurture_tick(&a, (int64_t)i * 250000, 0, false);
    nurture_tick(&b, 3600LL * 250000, 0, false);
    if (nurture_pct(a.satiety) != nurture_pct(b.satiety)) {
        printf("nurture: 高频步进 %u ≠ 单次步进 %u —— 余数被丢了\n",
               nurture_pct(a.satiety), nurture_pct(b.satiety));
        ok = false;
    }

    // ④b **不整除的间隔**也要等价。这条是对账脚本抓出来的，
    // ④ 抓不到 —— 250ms 恰好让 hours_q 长期为 0，只考验了
    // 「攒不够一个量子」那条路径。而真机的 tick 间隔是 3~8 秒，
    // hours_q = 1 但精确值是 1.7，那 0.7 个量子当时被直接丢掉了，
    // 跑一小时体能就比 PC 侧低 1.5。
    //
    // 6 秒 × 600 拍 = 1 小时，与单次跳 1 小时比。
    nurture_init(&a);
    nurture_init(&b);
    nurture_tick(&a, 0, 0, false);
    nurture_tick(&b, 0, 0, false);
    for (int i = 1; i <= 600; i++) nurture_tick(&a, (int64_t)i * 6000000, 0, false);
    nurture_tick(&b, 600LL * 6000000, 0, false);
    if (nurture_pct(a.stamina) != nurture_pct(b.stamina)) {
        printf("nurture: 6 秒步进体能 %u ≠ 单次 %u —— 量子余数被丢了\n",
               nurture_pct(a.stamina), nurture_pct(b.stamina));
        ok = false;
    }

    // ⑤ 下限不穿透
    nurture_init(&n);
    nurture_tick(&n, 0, 0, false);
    nurture_tick(&n, 100LL * US_PER_HOUR, 0, false);
    if (n.satiety != 0 || n.mood != 0) {
        printf("nurture: 100 小时后应触底，得到 %" PRId32 "/%" PRId32 "\n",
               n.satiety, n.mood);
        ok = false;
    }

    // ⑥ 心情分档与 sim 的 mood_label 一致
    nurture_init(&n);
    n.satiety = n.stamina = 90 * NURT_Q;
    n.mood = 85 * NURT_Q;
    if (nurture_mood(&n) != NURT_MOOD_HAPPY) { printf("nurture: 85 应愉快\n"); ok = false; }
    n.mood = 60 * NURT_Q;
    if (nurture_mood(&n) != NURT_MOOD_CALM) { printf("nurture: 60 应平静\n"); ok = false; }
    n.mood = 30 * NURT_Q;
    if (nurture_mood(&n) != NURT_MOOD_LOW) { printf("nurture: 30 应低落\n"); ok = false; }
    n.satiety = 10 * NURT_Q;          // 任一轴过低 → 消沉，压过 mood 档位
    n.mood = 90 * NURT_Q;
    if (nurture_mood(&n) != NURT_MOOD_DESPONDENT) {
        printf("nurture: 饱食 10 应消沉（不看 mood）\n");
        ok = false;
    }

    // ⑦ 上限不溢出
    nurture_init(&n);
    for (int i = 0; i < 10; i++) nurture_feed(&n);
    if (n.satiety != NURT_MAX) {
        printf("nurture: 喂 10 次应封顶，得到 %" PRId32 "\n", n.satiety);
        ok = false;
    }

    // ⑧ 三个主动照料动作与 sim/gameplay.py 一致，含副作用。
    nurture_init(&n);
    n.satiety = 50 * NURT_Q;
    n.mood = 40 * NURT_Q;
    n.stamina = 60 * NURT_Q;
    n.intimacy = 10 * NURT_Q;
    nurture_feed(&n);
    if (n.satiety != 80 * NURT_Q || n.mood != 45 * NURT_Q ||
        n.stamina != 60 * NURT_Q || n.intimacy != 10 * NURT_Q) {
        printf("nurture: 喂食结果不符 %u/%u/%u/%u\n",
               nurture_pct(n.satiety), nurture_pct(n.mood),
               nurture_pct(n.stamina), nurture_pct(n.intimacy));
        ok = false;
    }

    nurture_init(&n);
    n.mood = 40 * NURT_Q;
    n.stamina = 60 * NURT_Q;
    n.intimacy = 10 * NURT_Q;
    nurture_play(&n);
    if (n.mood != 55 * NURT_Q || n.stamina != 55 * NURT_Q ||
        n.intimacy != 11 * NURT_Q) {
        printf("nurture: 玩耍结果不符 %u/%u/%u\n",
               nurture_pct(n.mood), nurture_pct(n.stamina),
               nurture_pct(n.intimacy));
        ok = false;
    }

    nurture_init(&n);
    n.stamina = 30 * NURT_Q;
    nurture_rest(&n);
    if (n.stamina != 30 * NURT_Q) {
        printf("nurture: 休息结果不符 %u\n", nurture_pct(n.stamina));
        ok = false;
    }
    n.stamina = 80 * NURT_Q;
    nurture_rest(&n);
    if (n.stamina != 80 * NURT_Q) {
        printf("nurture: 休息不应回体力，得到 %u\n", nurture_pct(n.stamina));
        ok = false;
    }

    // 通过时也要出声 —— 静默通过与「根本没跑」在日志上没法区分。
    // 其余三个自检（assets / sensing / render）都打一行，这里对齐。
    //
    // 用 printf 而非 ESP_LOGI：这个文件要能在宿主上编译
    // （tools/pipeline/verify_nurture.py 直接 #include 它）。
    // 也不伪造 "I (123)" 的 ESP 日志前缀 —— 那会让日志过滤器误判。
    if (ok) {
        printf("nurture: 自检 全部通过"
               "（首拍 · 整点 · 幂等 · 高频等价 · 触底 · 分档 · 封顶 · 照料）\n");
    }
    return ok;
}

uint8_t nurture_exp_percent(const nurture_t *n) {
 if(!n)return 100;
 return 80+(nurture_pct(n->satiety)+nurture_pct(n->mood))/5+nurture_pct(n->intimacy)/10;
}
uint8_t nurture_rare_bonus(const nurture_t *n) {
 if(!n)return 0;
 return (nurture_pct(n->satiety)+nurture_pct(n->mood)+nurture_pct(n->intimacy))/6;
}

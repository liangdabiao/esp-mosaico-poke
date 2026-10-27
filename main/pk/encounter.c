// pk/encounter.c —— 遭遇 + 图鉴 + 闪光（自 ESP32-PokemonGo encounter.c 移植）。
//
// 与原版的差异：
//   · 种子源从 Wi-Fi BSSID 字符串换成 6 字节地图格子键
//     （map.c 生成：地图 id + 格子坐标）。格式化路径同构 ——
//     仍是「键字符串 | 小时桶」的 crc32，确定性不变。
//   · 删掉 AP 稀有度与后台扫描刷新（enc_refresh_*）：本项目由
//     踩格子即时触发遭遇，不再维护待处理队列的运营节奏。
//     队列结构保留（存档布局兼容、图鉴与自检共用）。
// 其余逻辑（按强度档选种、闪光独立 salt、FIFO 队列、图鉴位图）与
// 原版逐行一致 —— 那些是标定过的手感，不改。

#include <stdio.h>
#include <string.h>

#include "encounter.h"
#include "assets.h"
#include "pk_port.h"

#define TIME_BUCKET 3600      // 一小时一换
#define SHINY_DENOM 512       // 1/512，极稀有一半

// 强度档：种族值总和的区间。151 只分布：最低 175（绿毛虫）
// 中位 345 最高 590（超梦），四分位 275/345/420。
static const uint16_t TIER_LO[5] = {0, 250, 320, 400, 480};
static const uint16_t TIER_HI[5] = {250, 320, 400, 480, 9999};

// 6 字节位置键 → 小写十六进制串（与原版 BSSID 格式化同构）。
static int fmt_key(const uint8_t k[6], char *out)
{
    return snprintf(out, 18, "%02x%02x%02x%02x%02x%02x",
                    k[0], k[1], k[2], k[3], k[4], k[5]);
}

uint32_t enc_spawn_seed(const uint8_t key[6], uint32_t ts)
{
    char buf[48];
    char hex[18];
    fmt_key(key, hex);
    int n = snprintf(buf, sizeof(buf), "%s|%u", hex,
                     (unsigned)(ts / TIME_BUCKET));
    return pk_crc32(buf, (size_t)n);
}

bool enc_roll_shiny(const uint8_t key[6], uint32_t ts, uint8_t rarity)
{
    // 独立 salt 而非复用 spawn_seed 的低位 —— 否则闪光判定与种类判定
    // 相关，某些种类会永远不闪光。
    char buf[64];
    char hex[18];
    fmt_key(key, hex);
    int n = snprintf(buf, sizeof(buf), "%s|%u|shiny", hex,
                     (unsigned)(ts / TIME_BUCKET));
    uint32_t denom = (rarity >= 5) ? (SHINY_DENOM / 2) : SHINY_DENOM;
    return (pk_crc32(buf, (size_t)n) % denom) == 0;
}

uint16_t enc_pick_species(const uint8_t key[6], uint32_t ts, uint8_t rarity)
{
    // 按稀有度取物种池，不是均匀采样 151 只。
    uint8_t idx = (rarity >= 1 && rarity <= 5) ? (uint8_t)(rarity - 1) : 0;
    uint16_t lo = TIER_LO[idx], hi = TIER_HI[idx];

    // 先数池子大小，再取第 k 只。两趟扫描而不是先收集到数组。
    uint16_t n = 0;
    uint32_t total = assets_species_count();
    species_t sp;
    for (uint16_t id = 1; id <= total; id++) {
        if (!assets_species(id, &sp)) continue;
        uint16_t sum = (uint16_t)sp.hp + sp.attack + sp.defense
                     + sp.special + sp.speed;
        if (sum >= lo && sum < hi) n++;
    }
    if (n == 0) {
        uint32_t seed = enc_spawn_seed(key, ts);
        return (uint16_t)((seed >> 8) % (total ? total : 1) + 1);
    }

    uint32_t seed = enc_spawn_seed(key, ts);
    uint16_t k = (uint16_t)((seed >> 8) % n);
    for (uint16_t id = 1; id <= total; id++) {
        if (!assets_species(id, &sp)) continue;
        uint16_t sum = (uint16_t)sp.hp + sp.attack + sp.defense
                     + sp.special + sp.speed;
        if (sum >= lo && sum < hi) {
            if (k == 0) return id;
            k--;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 队列
// ---------------------------------------------------------------------------

void enc_queue_init(enc_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->next_uid = 1;          // 0 留作「无效/未选中」
}

encounter_t *enc_queue_find(enc_queue_t *q, uint16_t uid)
{
    if (!uid) return NULL;
    for (uint8_t i = 0; i < q->count; i++) {
        if (q->items[i].uid == uid) return &q->items[i];
    }
    return NULL;
}

bool enc_queue_take_uid(enc_queue_t *q, uint16_t uid, encounter_t *out)
{
    if (!uid) return false;
    for (uint8_t i = 0; i < q->count; i++) {
        if (q->items[i].uid != uid) continue;
        return enc_queue_take(q, i, out);
    }
    return false;
}

uint8_t enc_queue_trim(enc_queue_t *q)
{
    if (q->count <= ENC_QUEUE_LIMIT) return 0;
    uint8_t removed = q->count - ENC_QUEUE_LIMIT;
    memmove(q->items, q->items + removed, ENC_QUEUE_LIMIT * sizeof(q->items[0]));
    q->count = ENC_QUEUE_LIMIT;
    q->dropped += removed;
    memset(q->items + q->count, 0, (ENC_QUEUE_CAP - q->count) * sizeof(q->items[0]));
    return removed;
}

bool enc_queue_push(enc_queue_t *q, const encounter_t *e)
{
    // uid 跟着遭遇走，不跟下标。wrap 时跳过在队 uid，避免两条共用一个身份。
    do {
        if (q->next_uid == 0) q->next_uid = 1;
        if (!enc_queue_find(q, q->next_uid)) break;
        q->next_uid++;
    } while (true);
    uint16_t uid = q->next_uid++;
    bool dropped = enc_queue_trim(q) != 0;
    if (q->count == ENC_QUEUE_LIMIT) {
        enc_queue_take(q, 0, NULL);
        q->dropped++;
        dropped = true;
    }
    q->items[q->count] = *e;
    q->items[q->count].uid = uid;
    q->items[q->count].exp_granted = false;
    q->count++;
    return dropped;
}

bool enc_queue_take(enc_queue_t *q, uint8_t index, encounter_t *out)
{
    if (index >= q->count) return false;
    if (out) *out = q->items[index];
    for (uint8_t k = index; k + 1 < q->count; k++) q->items[k] = q->items[k + 1];
    q->count--;
    return true;
}

// ---------------------------------------------------------------------------
// 图鉴
// ---------------------------------------------------------------------------

static void bit_set(uint8_t *bm, uint16_t sid)
{
    if (sid < 1 || sid > DEX_SPECIES) return;
    uint16_t i = (uint16_t)(sid - 1);
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static bool bit_get(const uint8_t *bm, uint16_t sid)
{
    if (sid < 1 || sid > DEX_SPECIES) return false;
    uint16_t i = (uint16_t)(sid - 1);
    return (bm[i >> 3] >> (i & 7)) & 1;
}

static uint16_t bit_count(const uint8_t *bm)
{
    uint16_t n = 0;
    for (int i = 0; i < DEX_BYTES; i++) {
        uint8_t b = bm[i];
        while (b) { n += (uint16_t)(b & 1); b >>= 1; }
    }
    return n;
}

void dex_init(dex_t *d) { memset(d, 0, sizeof(*d)); }

void dex_mark_seen(dex_t *d, uint16_t sid, bool shiny)
{
    bit_set(d->seen, sid);
    if (shiny) bit_set(d->shiny_seen, sid);
}

void dex_mark_caught(dex_t *d, uint16_t sid, bool shiny)
{
    // 抓到必然见过 —— 两个位都置
    bit_set(d->seen, sid);
    bit_set(d->caught, sid);
    if (shiny) {
        bit_set(d->shiny_seen, sid);
        bit_set(d->shiny_caught, sid);
    }
}

bool dex_is_seen(const dex_t *d, uint16_t sid) { return bit_get(d->seen, sid); }
bool dex_is_caught(const dex_t *d, uint16_t sid) { return bit_get(d->caught, sid); }
bool dex_is_shiny_caught(const dex_t *d, uint16_t sid)
{
    return bit_get(d->shiny_caught, sid);
}
uint16_t dex_count_caught(const dex_t *d) { return bit_count(d->caught); }
uint16_t dex_count_seen(const dex_t *d) { return bit_count(d->seen); }

// ---------------------------------------------------------------------------

bool enc_selftest(void)
{
    bool ok = true;

    enc_queue_t q;
    enc_queue_init(&q);
    encounter_t e = {.rarity = 5, .species_id = 150, .ts = 1};
    enc_queue_push(&q, &e);
    uint16_t expired = q.items[0].uid;
    for (int i = 0; i < ENC_QUEUE_LIMIT; i++) {
        e.rarity = 1; e.species_id = (uint16_t)(10 + i); e.ts = (uint32_t)(2 + i);
        bool dropped = enc_queue_push(&q, &e);
        if (dropped != (i == ENC_QUEUE_LIMIT - 1)) ok = false;
    }
    if (q.count != ENC_QUEUE_LIMIT || q.dropped != 1 ||
        q.items[0].species_id != 10 || q.items[q.count - 1].species_id != 14 ||
        enc_queue_find(&q, expired)) {
        printf("encounter: FIFO did not keep the newest five\n");
        ok = false;
    }

    uint16_t watched = q.items[2].uid;
    enc_queue_take(&q, 0, NULL);
    encounter_t *found = enc_queue_find(&q, watched);
    encounter_t taken;
    if (!found || found != &q.items[1] || found->species_id != 12 ||
        !enc_queue_take_uid(&q, watched, &taken) || taken.species_id != 12) {
        printf("encounter: shifted uid no longer identifies its encounter\n");
        ok = false;
    }

    q.next_uid = q.items[0].uid;
    enc_queue_push(&q, &e);
    for (uint8_t i = 0; i < q.count; i++) {
        if (!q.items[i].uid) ok = false;
        for (uint8_t j = 0; j < i; j++)
            if (q.items[i].uid == q.items[j].uid) ok = false;
    }

    uint8_t before_n = q.count;
    if (enc_queue_take_uid(&q, 60000, NULL)) {
        printf("encounter: 不存在的 uid 竟然取成功了\n");
        ok = false;
    }
    if (q.count != before_n) {
        printf("encounter: 取失败却改了队列长度\n");
        ok = false;
    }

    enc_queue_init(&q);
    e.rarity = 5; e.species_id = 150; e.ts = 1;
    enc_queue_push(&q, &e);
    for (int i = 0; i < 3; i++) {
        e.rarity = 1; e.species_id = (uint16_t)(10 + i); e.ts = (uint32_t)(2 + i);
        enc_queue_push(&q, &e);
    }
    encounter_t got;
    uint8_t before = q.count;
    if (!enc_queue_take(&q, 0, &got) || q.count != before - 1) {
        printf("encounter: take 之后长度不对\n");
        ok = false;
    }

    dex_t d;
    dex_init(&d);
    dex_mark_seen(&d, 25, false);
    dex_mark_caught(&d, 1, true);
    if (!dex_is_seen(&d, 25) || dex_is_caught(&d, 25)) {
        printf("encounter: seen/caught 分不开\n");
        ok = false;
    }
    if (!dex_is_caught(&d, 1) || !dex_is_seen(&d, 1) ||
        !dex_is_shiny_caught(&d, 1)) {
        printf("encounter: 抓到应当同时置 seen 与 shiny\n");
        ok = false;
    }
    if (dex_count_seen(&d) != 2 || dex_count_caught(&d) != 1) {
        printf("encounter: 计数 seen=%u caught=%u，期望 2/1\n",
               dex_count_seen(&d), dex_count_caught(&d));
        ok = false;
    }
    dex_mark_caught(&d, 151, false);
    if (!dex_is_caught(&d, 151)) {
        printf("encounter: #151 存不进去 —— 位图边界错了\n");
        ok = false;
    }
    dex_mark_caught(&d, 152, false);
    if (dex_count_caught(&d) != 2) {
        printf("encounter: #152 越界没被挡住\n");
        ok = false;
    }

    // 确定性：同一位置键同一小时 → 同一种子、同一只
    static const uint8_t KEY[6] = {1, 2, 3, 4, 5, 6};
    if (enc_spawn_seed(KEY, 7200) != enc_spawn_seed(KEY, 7400) ||
        enc_spawn_seed(KEY, 7200) == enc_spawn_seed(KEY, 7201)) {
        printf("encounter: 种子不按位置+小时确定\n");
        ok = false;
    }
    if (enc_pick_species(KEY, 7200, 1) != enc_pick_species(KEY, 7300, 1)) {
        printf("encounter: 选种不按位置+小时确定\n");
        ok = false;
    }

    if (ok) {
        printf("encounter: 自检 全部通过"
               "（淘汰规则 · uid 稳定 · 取走 · 图鉴位图 · 边界 · 确定性）\n");
    }
    return ok;
}

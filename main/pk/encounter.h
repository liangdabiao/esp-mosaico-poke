// pk/encounter.h —— 遭遇队列 + 图鉴 + 闪光（自 ESP32-PokemonGo encounter.c/h 移植）。
//
// **这一整块是确定性的** —— 同一个位置键在同一小时永远刷出同一只，
// 靠 crc32 而不是随机数。三个好处（原 docs/03-spawning.md#36）：
// 跨设备一致、可预测（「这片草丛下午三点出火系」能做攻略）、
// 可重算（不用存刷新表）。
//
// 与原版的差异：种子源从 Wi-Fi BSSID 换成**地图格子键**
// （pk_map.h 生成 6 字节键：地图 id + 格子坐标）。确定性设计原样保留 ——
// 走回同一片草丛，同一小时刷的是同一批怪。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ENC_QUEUE_CAP 16
#define ENC_QUEUE_LIMIT 5
#define DEX_SPECIES 151
#define DEX_BYTES ((DEX_SPECIES + 7) / 8)    // 19

// 一条待处理遭遇。定长 —— 固件不做动态分配。
typedef struct {
    // 稳定标识。不能用队列下标认这一条 —— 后台入队/淘汰会整体移位。
    uint16_t uid;
    uint8_t level;
    uint8_t activity;

    uint32_t ts;
    uint16_t species_id;
    uint8_t rarity;           // 1=常见 5=极稀有
    uint8_t biome;
    uint8_t hp_ratio;         // 100 = 满血；打过之后降低，捕捉窗口用它
    bool is_shiny;
    bool is_transient;
    bool exp_granted;
} encounter_t;

typedef struct {
    encounter_t items[ENC_QUEUE_CAP];
    uint8_t count;
    uint16_t dropped;         // 统计：被挤掉了多少条
    uint16_t next_uid;        // 单调递增，0 保留作「无效」
} enc_queue_t;

// 图鉴四张位图。「闪光遇到了但跑了」那份遗憾要能存下来。
typedef struct {
    uint8_t seen[DEX_BYTES];
    uint8_t caught[DEX_BYTES];
    uint8_t shiny_seen[DEX_BYTES];
    uint8_t shiny_caught[DEX_BYTES];
} dex_t;

void enc_queue_init(enc_queue_t *q);
bool enc_queue_push(enc_queue_t *q, const encounter_t *e);
uint8_t enc_queue_trim(enc_queue_t *q);
bool enc_queue_take(enc_queue_t *q, uint8_t index, encounter_t *out);
bool enc_queue_take_uid(enc_queue_t *q, uint16_t uid, encounter_t *out);
encounter_t *enc_queue_find(enc_queue_t *q, uint16_t uid);

// 闪光判定 —— crc32，确定性。极稀有（rarity ≥ 5）翻倍概率。
bool enc_roll_shiny(const uint8_t key[6], uint32_t ts, uint8_t rarity);

// 确定性刷新种子：同一位置键同一小时永远同一个值。
uint32_t enc_spawn_seed(const uint8_t key[6], uint32_t ts);

// 按稀有度从对应强度档挑一只。**不是均匀采样 151 只** ——
// 均匀采样会让 Lv12 的初期主宠遇到超梦。
uint16_t enc_pick_species(const uint8_t key[6], uint32_t ts, uint8_t rarity);

// 图鉴
void dex_init(dex_t *d);
void dex_mark_seen(dex_t *d, uint16_t sid, bool shiny);
void dex_mark_caught(dex_t *d, uint16_t sid, bool shiny);
bool dex_is_seen(const dex_t *d, uint16_t sid);
bool dex_is_caught(const dex_t *d, uint16_t sid);
bool dex_is_shiny_caught(const dex_t *d, uint16_t sid);
uint16_t dex_count_caught(const dex_t *d);
uint16_t dex_count_seen(const dex_t *d);

bool enc_selftest(void);

// main/battle.h —— S3 自动战斗（属性相克 + 伤害 + 回合循环）。
//
// PC 侧是 sim/systems.py。**确定性部分逐值对账**
// （tools/pipeline/verify_battle.py），随机部分只对统计分布 ——
// sim 用 Python 的 Mersenne Twister，C 里复刻不现实，
// 而战斗的观感取决于胜率分布而不是某一场的逐回合复现。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "assets.h"
#include "combat.h"

#define BATTLE_MAX_ROUNDS 40      // 与 sim 的 max_rounds 默认值一致
#define BATTLE_TYPE_COUNT 17      // 金银属性；前15个下标保持素材兼容

// 属性下标 —— 与 tools/pipeline/convert_gen1.py 的 GEN1_TYPES 同序，
// 也与 sim/strings.py 的 TYPES_CN 逐位对应（已验证三处一致）。
// gen1.bin 里存的就是这个下标。
enum {
    TY_NORMAL = 0, TY_FIRE, TY_WATER, TY_ELECTRIC, TY_GRASS, TY_ICE,
    TY_FIGHTING, TY_POISON, TY_GROUND, TY_FLYING, TY_PSYCHIC, TY_BUG,
    TY_ROCK, TY_GHOST, TY_DRAGON, TY_DARK, TY_STEEL,
};
#define TY_NONE 0xFF

// 一个回合的记录 —— P3 逐回合播放要用
typedef struct battle_round {
    uint8_t by_pet;           // 1 = 主宠出手
    uint16_t damage;
    uint16_t mult;            // 倍率 ×100
    uint16_t pet_hp, wild_hp;
    uint16_t move_id;         // Stable presentation dispatch key, never localized text.
    uint8_t move_type;        // TY_*; TY_NONE when no move was selected.
    const char *move_zh;      // 指向字符串池，非 NUL 结尾
    uint8_t move_zh_len;
    bool missed;
    uint8_t self_target, no_effect, charging, skipped, hits, critical;
    uint16_t healed;
    uint8_t fatigue;
} battle_round_t;

typedef struct {
    bool won;
    uint8_t round_count;
    battle_round_t rounds[BATTLE_MAX_ROUNDS];
    uint16_t exp;
    uint8_t wild_hp_ratio;    // 传给 S2 —— 打残了更好抓。**最低 1 不为 0**
    uint16_t pet_hp_max, wild_hp_max;
} battle_result_t;

// A resumable encounter battle. No pointers or animation clocks: copies retain
// exact HP, turn order and RNG across P3/P4 and encounter-list navigation.
typedef struct {
    uint32_t rng;
    uint16_t pet_species, wild_species;
    uint16_t pet_hp, wild_hp, pet_hp_max, wild_hp_max;
    uint16_t ability_factor_q10;
    uint8_t pet_level, wild_level, attack_count;
    uint8_t escape_attempts;
    bool initialized, started, finished, won;
    // Only the explicit Battle choice enables the continuous turn loop.
    // A failed capture can start one attack while auto_battle stays false.
    bool auto_battle;
    bool intro_seen;
    bool next_by_pet;
    bool retaliation_pending;
    bool escape_retaliation;
    bool capture_used_after_win;
    bool reward_settled;
    bool defeat_applied;
    bool loot_checked, loot_full;
    uint8_t loot_item, loot_qty;
    combat_mon_t fighters[2];
    uint8_t acted;
    uint16_t planned[2];
    // Mosaico 触摸版扩展：玩家指定招式。置 player_move 后下一步
    // 计划阶段用它替代 combat_choose 的自动选择（仅本回合生效）。
    uint16_t player_move;
    bool player_move_set;
} battle_session_t;

bool battle_session_init(battle_session_t *session,
                          uint16_t pet_species, uint8_t pet_level,
                          uint16_t wild_species, uint8_t wild_level,
                          uint16_t ability_factor_q10, uint32_t seed);
// Commit one attack independently of frame timing. Pending capture retaliation
// forces one wild attack, then gives the next ordinary turn to the player.
bool battle_session_step(battle_session_t *session, battle_round_t *out);
uint8_t battle_session_hp_ratio(const battle_session_t *session);
uint16_t battle_session_exp(const battle_session_t *session);
bool battle_session_can_capture(const battle_session_t *session);

// 属性相克倍率（×100）。双属性相乘；def2 传 TY_NONE 表示单属性。
uint16_t battle_effectiveness(uint8_t atk, uint8_t def1, uint8_t def2);

// 倍率 → 文案。100 返回 NULL（**正常倍率不提示** ——
// 每回合都弹「效果一般」会把「效果绝佳」的分量冲掉，见 P3 页面文档）。
const char *battle_eff_label(uint16_t mult);

// Gen-II non-HP stat; fixed DV 15, stat experience 0 (same policy as combat_stat).
uint16_t battle_effective_stat(uint8_t base, uint8_t level);

// Baseline bands and progression-aware level (freeze in battle_session_t).
uint8_t battle_wild_level(uint8_t rarity);
uint8_t battle_wild_level_for_pet(uint8_t rarity, uint8_t pet_level);

// 打一场。seed 让同一组输入得到同一场战斗（可回放）。
void battle_run(uint16_t pet_species, uint8_t pet_level,
                uint16_t wild_species, uint8_t wild_level,
                uint16_t ability_factor_q10,   // 消沉时 0.6 → 614
                uint32_t seed,
                battle_result_t *out);

// 与 PC 侧对账。宿主上编译运行，见 tools/pipeline/verify_battle.py。
bool battle_selftest(void);

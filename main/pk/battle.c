// Wild battles share the generation-II stats and damage core with trainer battles.
#include <string.h>

#include "battle.h"
#include "exp.h"

// ---------------------------------------------------------------------------
// 属性相克
// ---------------------------------------------------------------------------

// Gold/Silver chart; first 15 indices preserve existing asset IDs.
static const uint8_t EFF[BATTLE_TYPE_COUNT][BATTLE_TYPE_COUNT] = {
 {100,100,100,100,100,100,100,100,100,100,100,100,50,0,100,100,50},
 {100,50,50,100,200,200,100,100,100,100,100,200,50,100,50,100,200},
 {100,200,50,100,50,100,100,100,200,100,100,100,200,100,50,100,100},
 {100,100,200,50,50,100,100,100,0,200,100,100,100,100,50,100,100},
 {100,50,200,100,50,100,100,50,200,50,100,50,200,100,50,100,50},
 {100,50,50,100,200,50,100,100,200,200,100,100,100,100,200,100,50},
 {200,100,100,100,100,200,100,50,100,50,50,50,200,0,100,200,200},
 {100,100,100,100,200,100,100,50,50,100,100,100,50,50,100,100,0},
 {100,200,100,200,50,100,100,200,100,0,100,50,200,100,100,100,200},
 {100,100,100,50,200,100,200,100,100,100,100,200,50,100,100,100,50},
 {100,100,100,100,100,100,200,200,100,100,50,100,100,100,100,0,50},
 {100,50,100,100,200,100,50,50,100,50,200,100,100,50,100,200,50},
 {100,200,100,100,100,200,50,100,50,200,100,200,100,100,100,100,50},
 {0,100,100,100,100,100,100,100,100,100,200,100,100,200,100,50,50},
 {100,100,100,100,100,100,100,100,100,100,100,100,100,100,200,100,50},
 {100,100,100,100,100,100,50,100,100,100,200,100,100,200,100,50,50},
 {100,50,50,50,100,200,100,100,100,100,100,100,200,100,100,100,50},

};

#define STAB 150              // 本属性加成 ×1.5（初代原版就是这个数）
#define ACC_ALWAYS_HIT 255    // moves.bin 里 255 = 必中（高速星星）

uint16_t battle_effectiveness(uint8_t atk, uint8_t def1, uint8_t def2)
{
    if (atk >= BATTLE_TYPE_COUNT) return 100;
    uint16_t mult = 100;
    if (def1 < BATTLE_TYPE_COUNT) mult = mult * EFF[atk][def1] / 100;
    if (def2 < BATTLE_TYPE_COUNT) mult = mult * EFF[atk][def2] / 100;
    return mult;
}

const char *battle_eff_label(uint16_t mult)
{
    if (mult == 0) return "没有效果…";
    if (mult >= 200) return "效果绝佳！";
    if (mult <= 50) return "效果不好…";
    return NULL;              // 正常倍率**不提示**，见 battle.h
}

uint16_t battle_effective_stat(uint8_t base, uint8_t level)
{
    // Generation II, fixed DV 15 and no stat experience on either side.
    return (2u * (base + 15u) * level) / 100u + 5u;
}

uint8_t battle_wild_level(uint8_t rarity)
{
    static const uint8_t BAND[6] = {12, 5, 12, 20, 30, 45};
    return rarity <= 5 ? BAND[rarity] : 12;
}

uint8_t battle_wild_level_for_pet(uint8_t rarity, uint8_t pet_level)
{
    // Ordinary exploration scales gently, including rare early-game finds.
    // Badge expeditions have their own explicit recommended levels.
    static const uint8_t PERCENT[6] = {90, 85, 90, 95, 100, 105};
    uint8_t tier = rarity <= 5 ? rarity : 0;
    unsigned level = (pet_level > 100 ? 100 : pet_level) * PERCENT[tier] / 100;
    unsigned floor = 2;
    if (level < floor) level = floor;
    return level > 100 ? 100 : level;
}

bool battle_session_init(battle_session_t *s,
                          uint16_t pet_species, uint8_t pet_level,
                          uint16_t wild_species, uint8_t wild_level,
                          uint16_t ability_factor_q10, uint32_t seed)
{
    if (!s || !pet_level || pet_level>100 || !wild_level || wild_level>100) return false;
    memset(s, 0, sizeof(*s));
    species_t pet_sp, wild_sp;
    if (!assets_species(pet_species, &pet_sp) ||
        !assets_species(wild_species, &wild_sp)) return false;
    s->pet_species = pet_species;
    s->wild_species = wild_species;
    s->pet_level = pet_level;
    s->wild_level = wild_level;
    s->pet_hp = s->pet_hp_max = combat_max_hp(pet_species,pet_level);
    s->wild_hp = s->wild_hp_max = combat_max_hp(wild_species,wild_level);
    s->ability_factor_q10 = ability_factor_q10;
    s->rng = seed ? seed : 1;
    combat_init(&s->fighters[0],pet_species,pet_level,s->pet_hp_max);
    combat_init(&s->fighters[1],wild_species,wild_level,s->wild_hp_max);
    s->next_by_pet = combat_speed(&s->fighters[0]) >= combat_speed(&s->fighters[1]);
    s->initialized = true;
    return true;
}

uint8_t battle_session_hp_ratio(const battle_session_t *s)
{
    if (!s || !s->initialized || !s->wild_hp_max) return 100;
    uint32_t ratio = (uint32_t)s->wild_hp * 100 / s->wild_hp_max;
    return (uint8_t)(ratio < 1 ? 1 : ratio > 100 ? 100 : ratio);
}

uint16_t battle_session_exp(const battle_session_t *s)
{
    return s && s->initialized && s->finished
        ? (uint16_t)(exp_battle_base(s->wild_level) * (s->won ? 100 : 30) / 100) : 0;
}

bool battle_session_can_capture(const battle_session_t *s)
{
    if (!s || !s->initialized || s->retaliation_pending) return false;
    return s->finished ? s->won && !s->capture_used_after_win
                       : !s->auto_battle && s->pet_hp > 0;
}

bool battle_session_step(battle_session_t *s, battle_round_t *out)
{
    if (!s || !out || !s->initialized || s->finished) return false;
    memset(out, 0, sizeof(*out));
    species_t pet_sp, wild_sp;
    if (!assets_species(s->pet_species, &pet_sp) ||
        !assets_species(s->wild_species, &wild_sp)) return false;
    if (!s->pet_hp || !s->wild_hp || s->attack_count >= BATTLE_MAX_ROUNDS) return false;
    s->fighters[0].hp=s->pet_hp;s->fighters[1].hp=s->wild_hp;
    if((s->acted==0||s->acted==3)&&!s->retaliation_pending){
        if(s->player_move_set){s->planned[0]=s->player_move;s->player_move=0;s->player_move_set=false;}
        else s->planned[0]=combat_choose(&s->fighters[0],&s->fighters[1],&s->rng);
        s->planned[1]=combat_choose(&s->fighters[1],&s->fighters[0],&s->rng);
        int priority=combat_priority(s->planned[0])-combat_priority(s->planned[1]);
        s->next_by_pet=priority?priority>0:combat_speed(&s->fighters[0])>=combat_speed(&s->fighters[1]);s->acted=0;
    }
    bool retaliation=s->retaliation_pending;
    bool by_pet = !s->retaliation_pending && s->next_by_pet;
    combat_mon_t *a=&s->fighters[by_pet?0:1],*d=&s->fighters[by_pet?1:0];
    // HP mirrors remain the capture/escape API; all effects use the same core.
    s->fighters[0].hp=s->pet_hp;s->fighters[1].hp=s->wild_hp;
    out->by_pet=by_pet;
    combat_turn(a,d,by_pet?s->ability_factor_q10:1024,&s->rng,50,retaliation?0:s->planned[by_pet?0:1],out);
    s->acted|=by_pet?1:2;
    if(!retaliation&&s->acted==3)combat_finish_round(&s->fighters[0],&s->fighters[1],out);
    s->pet_hp=s->fighters[0].hp;s->wild_hp=s->fighters[1].hp;
    if(s->acted&(by_pet?2:1))d->flinch=0;
    if(retaliation)s->acted=0;
    s->next_by_pet = !by_pet;
    s->retaliation_pending = false;
    s->started = true;
    s->attack_count++;
    s->finished = !s->pet_hp || !s->wild_hp || s->attack_count >= BATTLE_MAX_ROUNDS;
    s->won = s->finished && !s->wild_hp && s->pet_hp > 0;
    out->pet_hp = s->pet_hp;
    out->wild_hp = s->wild_hp;
    return true;
}

void battle_run(uint16_t pet_species, uint8_t pet_level,
                uint16_t wild_species, uint8_t wild_level,
                uint16_t ability_factor_q10, uint32_t seed,
                battle_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    battle_session_t session;
    if (!battle_session_init(&session, pet_species, pet_level,
                             wild_species, wild_level, ability_factor_q10, seed)) return;
    out->pet_hp_max = session.pet_hp_max;
    out->wild_hp_max = session.wild_hp_max;
    while (out->round_count < BATTLE_MAX_ROUNDS &&
           battle_session_step(&session, &out->rounds[out->round_count])) {
        out->round_count++;
    }
    out->won = session.won;
    out->wild_hp_ratio = battle_session_hp_ratio(&session);
    out->exp = battle_session_exp(&session);
}

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "assets.h"
#define COMBAT_MOVE_CAP 191
#define COMBAT_MAX_MOVE_ID 250
// Legacy prefix retained for save compatibility; no stored repertoire or PP economy.
typedef struct {
 uint16_t hp,max_hp;
 // Reuse the retired move slots without changing the persisted struct layout.
 // Legacy IDs never equal the safety marker; old battles initialize lazily.
 union {
  uint16_t moves[4];
  struct {uint16_t marker,low_hp;uint8_t stalled,fatigue,status_streak;int8_t special_defense;} safety;
 };
 uint8_t species,level,pp[4],status,sleep,seeded;
 int8_t attack,defense,special,speed;
 uint16_t substitute,last_damage,last_move,charge_move,bide_damage,disabled_move;
 uint8_t confusion,toxic,trap,recharge,charge,bide,flinch,mist,focus,rage;
 uint8_t reflect,light_screen,transform_species,transform_level,disable_turns;
 int8_t accuracy,evasion;
 uint8_t converted,converted_type;
 uint16_t mimic_move;
} combat_mon_t;
struct battle_round;
bool combat_move(uint16_t id, move_t *out);
int combat_known_moves(uint16_t species,uint8_t level,uint16_t *out,int capacity);
uint8_t combat_learn_level(uint16_t species,uint16_t move);
void combat_init(combat_mon_t *m,uint8_t species,uint8_t level,uint16_t hp);
uint16_t combat_speed(const combat_mon_t *m);
enum { COMBAT_HP,COMBAT_ATTACK,COMBAT_DEFENSE,COMBAT_SPEED,COMBAT_SP_ATTACK,COMBAT_SP_DEFENSE };
uint16_t combat_stat(uint8_t species,uint8_t level,unsigned stat);
uint16_t combat_max_hp(uint8_t species,uint8_t level);
int8_t combat_sp_def_stage(const combat_mon_t *m);
// V12 and older saved trainer battles: preserve HP fraction and fainted state.
void combat_migrate_gen2(combat_mon_t *m);
void combat_reset_volatile(combat_mon_t *m);
bool combat_valid(const combat_mon_t *m);
uint16_t combat_choose(const combat_mon_t *a,const combat_mon_t *d,uint32_t *rng);
void combat_turn(combat_mon_t *a,combat_mon_t *d,uint16_t ability,uint32_t *rng,
                 unsigned divisor,uint16_t forced_move,struct battle_round *out);

const char *combat_description(uint16_t id);
const char *combat_feedback(const struct battle_round *round);

int combat_priority(uint16_t move);

void combat_finish_round(combat_mon_t *a,combat_mon_t *d,struct battle_round *out);

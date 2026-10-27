#pragma once

#include <stdint.h>
#include <stdbool.h>

#define LEVEL_MAX       100
#define EXP_ON_CAPTURE   60
#define EXP_ON_CARE      30
#define EXP_ON_MOTION     8

uint32_t exp_for_level(uint8_t n);
uint8_t exp_to_level(uint32_t exp, uint8_t cap);
void exp_progress(uint32_t exp, uint8_t level, uint32_t *got, uint32_t *need);

uint16_t exp_scaled(uint16_t base,uint8_t percent);

#include "party.h"
void exp_share_party(party_t *,unsigned eligible,uint16_t award);

uint16_t exp_battle_base(uint8_t level);
unsigned exp_party_percent(uint8_t level,uint8_t highest,bool participant);
void exp_award_party(party_t *,unsigned participants,unsigned eligible,uint16_t award);

// Presentation events contain no progression state. Publish only after an
// award is committed; a failed save or a party swap must not create a notice.
typedef struct { uint8_t species, before, after, flags, slot; } exp_growth_t;
#define EXP_GROWTH_CAP (PARTY_MAX * 2)
typedef struct { exp_growth_t events[EXP_GROWTH_CAP]; uint8_t count; } exp_growth_queue_t;
void exp_growth_record(exp_growth_queue_t *,const mon_t *before,unsigned before_count,
                       const mon_t *after,unsigned after_count);
bool exp_growth_pop(exp_growth_queue_t *,exp_growth_t *);

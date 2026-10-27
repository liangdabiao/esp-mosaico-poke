#pragma once
#include "battle.h"

typedef struct {
    bool escaped;
    uint16_t chance; // Successful byte outcomes, 1..256.
} battle_escape_result_t;

// Crystal's speed check and +30 threshold per previous failed attempt.
// Uses wide arithmetic; does not reproduce the original 8-bit overflow bugs.
uint16_t battle_escape_chance(uint16_t pet_speed, uint16_t wild_speed,
                              uint8_t previous_attempts);
bool battle_escape_try(battle_session_t *session, battle_escape_result_t *out);

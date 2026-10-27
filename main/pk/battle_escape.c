// Reference: pret/pokecrystal 7a7881d0d62e0ddbd82dcf10e7116807487ac651,
// engine/battle/core.asm::TryToRunAwayFromBattle. Uses Gen-II non-HP stats
// with fixed DV 15 / stat EXP 0 and the original successful-byte threshold.
#include "battle_escape.h"
#include <string.h>

uint16_t battle_escape_chance(uint16_t pet_speed, uint16_t wild_speed,
                              uint8_t previous_attempts)
{
    if (pet_speed >= wild_speed || wild_speed < 4) return 256;
    uint32_t threshold = (uint32_t)pet_speed * 32 / (wild_speed / 4);
    threshold += (uint32_t)previous_attempts * 30;
    // Crystal succeeds on random_byte <= threshold (not strictly less).
    return threshold >= 255 ? 256 : (uint16_t)(threshold + 1);
}

bool battle_escape_try(battle_session_t *s, battle_escape_result_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s || !s->initialized || s->finished || !s->pet_hp ||
        s->retaliation_pending) return false;
    species_t pet, wild;
    if (!assets_species(s->pet_species, &pet) ||
        !assets_species(s->wild_species, &wild)) return false;
    out->chance = battle_escape_chance(battle_effective_stat(pet.speed, s->pet_level),
        battle_effective_stat(wild.speed, s->wild_level), s->escape_attempts);
    if (s->escape_attempts < 255) s->escape_attempts++;
    s->started = true;
    out->escaped = out->chance == 256;
    if (!out->escaped) {
        uint32_t x = s->rng ? s->rng : 1;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        s->rng = x;
        out->escaped = (x & 255) < out->chance;
    }
    s->escape_retaliation = !out->escaped;
    s->retaliation_pending = !out->escaped;
    return true;
}

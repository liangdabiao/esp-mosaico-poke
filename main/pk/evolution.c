#include "evolution.h"

bool evo_level_ready(uint8_t level, uint8_t trigger, uint16_t target, uint8_t required)
{
    return trigger == EVO_TRIGGER_LEVEL && target > 0 && target <= 151 &&
           required > 0 && required <= 100 && level >= required;
}

#define DEFAULT_INTIMACY 60
#define DEFAULT_EXPLORE_MIN 10
#define DEFAULT_EXPLORE_MULT 2
#define TRADE_INTIMACY 90
#define TRADE_EXPLORE_MIN 20
#define TRADE_EXPLORE_MULT 4

void evo_check(uint8_t intimacy_pct, uint16_t explore_value,
               uint8_t trigger, uint8_t evolve_to, uint8_t evolve_level,
               evo_check_t *out)
{
    if (!out) return;

    *out = (evo_check_t){
        .cur_intimacy = intimacy_pct,
        .cur_explore = explore_value,
    };
    if (trigger == EVO_TRIGGER_NONE || evolve_to == 0) return;

    uint8_t need_intimacy = DEFAULT_INTIMACY;
    uint16_t need_explore = (uint16_t)evolve_level * DEFAULT_EXPLORE_MULT;
    if (need_explore < DEFAULT_EXPLORE_MIN) need_explore = DEFAULT_EXPLORE_MIN;

    if (trigger == EVO_TRIGGER_TRADE) {
        need_intimacy = TRADE_INTIMACY;
        need_explore = (uint16_t)evolve_level * TRADE_EXPLORE_MULT;
        if (need_explore < TRADE_EXPLORE_MIN) need_explore = TRADE_EXPLORE_MIN;
    }

    out->need_intimacy = need_intimacy;
    out->need_explore = need_explore;
    out->can = intimacy_pct >= need_intimacy && explore_value >= need_explore;
}

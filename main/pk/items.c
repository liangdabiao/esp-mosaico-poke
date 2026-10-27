#include "items.h"
#include "assets.h"
#include "evolution.h"
#include <string.h>

static const item_info_t INFO[ITEM_COUNT] = {
    {"精灵球", "基本捕获球", ITEM_KIND_BALL},
    {"超级球", "捕获窗口增至一点五倍", ITEM_KIND_BALL},
    {"高级球", "捕获窗口增至两倍", ITEM_KIND_BALL},
    {"大师球", "必定捕获", ITEM_KIND_BALL},
    {"速度球", "对方基础速度达一百时四倍", ITEM_KIND_BALL},
    {"沉重球", "百公斤两倍，两百公斤三倍", ITEM_KIND_BALL},
    {"等级球", "等级较高一点五倍，达两倍则两倍", ITEM_KIND_BALL},
    {"友友球", "捕获后亲密度为四十", ITEM_KIND_BALL},
    {"火之石", "使对应宝可梦进化", ITEM_KIND_STONE},
    {"水之石", "使对应宝可梦进化", ITEM_KIND_STONE},
    {"雷之石", "使对应宝可梦进化", ITEM_KIND_STONE},
    {"叶之石", "使对应宝可梦进化", ITEM_KIND_STONE},
    {"月之石", "使对应宝可梦进化", ITEM_KIND_STONE},
    {"通讯机器", "替代通讯交换完成进化", ITEM_KIND_MACHINE},
    {"成长机器", "达到原进化等级即可使用", ITEM_KIND_MACHINE},
    {"树果", "饱食加三十，心情加五", ITEM_KIND_CARE},
    {"活力根", "饱食加十，心情加二十", ITEM_KIND_CARE},
    {"哞哞牛奶", "饱食加二十，心情加十", ITEM_KIND_CARE},
    {"开心饼干", "心情加二十五，亲密加二", ITEM_KIND_CARE},
    {"回复水", "回复二十体力", ITEM_KIND_HEAL},
    {"高级回复水", "回复五十体力", ITEM_KIND_HEAL},
    {"全复水", "体力全部回复", ITEM_KIND_HEAL},
};
static const uint8_t CAPACITY[ITEM_COUNT] = {
    99,30,20,1,20,20,20,20, 9,9,9,9,9, 5,5, 30,30,30,30, 30,20,10,
};

const item_info_t *items_info(uint8_t id) { return id < ITEM_COUNT ? &INFO[id] : NULL; }
uint16_t items_capacity(uint8_t id) { return id < ITEM_COUNT ? CAPACITY[id] : 0; }

void items_inventory_init(inventory_t *inventory)
{
    if (!inventory) return;
    memset(inventory, 0, sizeof(*inventory));
    inventory->quantity[ITEM_POKE] = 12;
    inventory->quantity[ITEM_GREAT] = 3;
    inventory->quantity[ITEM_ULTRA] = 1;
    inventory->quantity[ITEM_BERRY] = 2;
    inventory->quantity[ITEM_MILK] = 2;
}

bool items_inventory_valid(const inventory_t *inventory)
{
    if (!inventory) return false;
    for (unsigned i = 0; i < ITEM_COUNT; i++)
        if (inventory->quantity[i] > CAPACITY[i]) return false;
    return true;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

uint8_t items_drop_chance(uint8_t rarity)
{
    return rarity >= 1 && rarity <= 5 ? (uint8_t)(15 + rarity * 10) : 0;
}

uint32_t items_loot_seed(uint16_t uid, uint32_t ts, uint16_t species, uint8_t rarity)
{
    uint32_t value = ts ^ ((uint32_t)uid << 16) ^ ((uint32_t)species * 0x9E3779B9u)
        ^ ((uint32_t)rarity * 0x85EBCA6Bu);
    value ^= value >> 16; value *= 0x7FEB352Du;
    value ^= value >> 15; value *= 0x846CA68Bu;
    return value ^ (value >> 16);
}

item_loot_t items_roll_loot(uint8_t rarity, uint32_t seed)
{
    item_loot_t out = {.item_id = ITEM_NONE};
    if (rarity < 1 || rarity > 5) return out;
    // Separately mixed stream, unrelated to the battle's xorshift state.
    uint32_t rng = seed ^ 0xA511E9B3u;
    if (!rng) rng = 0x6D2B79F5u;
    if (next_random(&rng) % 100 >= items_drop_chance(rarity)) {
        out.item_id=next_random(&rng)%4?ITEM_POKE:ITEM_BERRY;
        out.quantity=out.item_id==ITEM_POKE?2:1;
        return out;
    }
    static const uint8_t weights[5][ITEM_COUNT] = {
        {40,12, 2,0, 2,2,2,2, 1,1,1,1,1, 0,0, 24,5,12,5,  12,0,0},
        {32,17, 5,0, 3,3,3,3, 2,2,2,2,2, 1,1, 18,6,12,6,  14,2,0},
        {23,20, 9,0, 5,5,5,5, 3,3,3,3,3, 2,2, 14,7,11,7,  14,5,1},
        {15,20,14,0, 6,6,6,6, 5,5,5,5,5, 4,4, 10,7,10,7,  12,8,3},
        { 8,16,20,1, 8,8,8,8, 7,7,7,7,7, 6,6,  7,7, 8,7,   8,10,6},
    };
    unsigned total = 0;
    for (unsigned i = 0; i < ITEM_COUNT; i++) total += weights[rarity-1][i];
    unsigned pick = next_random(&rng) % total;
    for (unsigned i = 0; i < ITEM_COUNT; i++) {
        if (pick < weights[rarity-1][i]) {
            out.item_id = i;
            out.quantity = i == ITEM_POKE ? (uint8_t)(2 + next_random(&rng) % 2) : 1;
            return out;
        }
        pick -= weights[rarity-1][i];
    }
    return out;
}

static int32_t add_axis(int32_t before, int amount)
{
    int64_t result = (int64_t)before + amount * NURT_Q;
    return result < 0 ? 0 : result > NURT_MAX ? NURT_MAX : (int32_t)result;
}

static bool info_kind_is_heal(uint8_t id)
{
    return id >= ITEM_POTION && id <= ITEM_MAX_POTION;
}

item_use_status_t items_apply(uint8_t id, uint16_t species, uint8_t level,
                              const nurture_t *before, item_use_result_t *out)
{
    if (!before || !out || id >= ITEM_COUNT || species < 1 || species > 151)
        return ITEM_USE_INVALID;
    *out = (item_use_result_t){.species_before = species, .species_after = species,
                             .before = *before, .after = *before};
    if (id < ITEM_BALL_COUNT) return ITEM_USE_NOT_APPLICABLE;
    // Gen1 stone targets are explicit, including all three Eevee branches.
    static const uint8_t stone_targets[][3] = {
        {ITEM_FIRE_STONE,37,38},{ITEM_FIRE_STONE,58,59},{ITEM_FIRE_STONE,133,136},
        {ITEM_WATER_STONE,61,62},{ITEM_WATER_STONE,90,91},{ITEM_WATER_STONE,120,121},{ITEM_WATER_STONE,133,134},
        {ITEM_THUNDER_STONE,25,26},{ITEM_THUNDER_STONE,133,135},
        {ITEM_LEAF_STONE,44,45},{ITEM_LEAF_STONE,70,71},{ITEM_LEAF_STONE,102,103},
        {ITEM_MOON_STONE,30,31},{ITEM_MOON_STONE,33,34},{ITEM_MOON_STONE,35,36},{ITEM_MOON_STONE,39,40},
    };
    if (id <= ITEM_MOON_STONE) {
        for (unsigned i = 0; i < sizeof(stone_targets)/sizeof(stone_targets[0]); i++)
            if (stone_targets[i][0] == id && stone_targets[i][1] == species)
                out->species_after = stone_targets[i][2];
        if (out->species_after == species) return ITEM_USE_NOT_APPLICABLE;
    } else if (id == ITEM_LINK_MACHINE) {
        if (species == 64 || species == 67 || species == 75 || species == 93)
            out->species_after = species + 1;
        else return ITEM_USE_NOT_APPLICABLE;
    } else if (id == ITEM_GROWTH_MACHINE) {
        species_t sp;
        if (!assets_species(species, &sp) || sp.evolve_trigger != EVO_TRIGGER_LEVEL ||
            !sp.evolve_to || !sp.evolve_level) return ITEM_USE_NOT_APPLICABLE;
        if (level < sp.evolve_level) return ITEM_USE_LEVEL_TOO_LOW;
        out->species_after = sp.evolve_to;
    } else if (info_kind_is_heal(id)) {
        // 回复类：只做资格判定（背包里有、目标合法），HP 由游戏层结算。
        return ITEM_USE_OK;
    } else {
        int satiety = 0, mood = 0, stamina = 0, intimacy = 0;
        switch (id) {
        case ITEM_BERRY: satiety = 30; mood = 5; break;
        case ITEM_ENERGY_ROOT: satiety = 10; mood = 20; break;
        case ITEM_MILK: satiety = 20; mood = 10; break;
        case ITEM_JOY_COOKIE: mood = 25; intimacy = 2; break;
        default: return ITEM_USE_INVALID;
        }
        // Do not spend food with no positive benefit (or only a side effect).
        bool useful = (satiety > 0 && before->satiety < NURT_MAX) ||
            (mood > 0 && before->mood < NURT_MAX) ||
            (stamina > 0 && before->stamina < NURT_MAX) ||
            (intimacy > 0 && before->intimacy < NURT_MAX);
        if (!useful) return ITEM_USE_NOT_APPLICABLE;
        out->after.satiety = add_axis(before->satiety, satiety);
        out->after.mood = add_axis(before->mood, mood);
        out->after.stamina = add_axis(before->stamina, stamina);
        out->after.intimacy = add_axis(before->intimacy, intimacy);
    }
    if (out->species_after != species)
        out->after.mood = add_axis(before->mood, 15); // Existing evolution bonus.
    return ITEM_USE_OK;
}

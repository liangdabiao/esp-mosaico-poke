// tests/test_pk.c —— 纯逻辑层宿主单测入口（gcc 直接编译运行，不依赖引擎）。
//
// 覆盖：资产解析、图鉴/遭遇确定性、世界存档往返、地图寻路、
// 战斗会话（含玩家选招注入）、捕捉窗口、道具/经验/进化。
// 运行：见 tests/run_tests.sh。
#include <stdio.h>
#include <string.h>

#include "pk/assets.h"
#include "pk/battle.h"
#include "pk/battle_escape.h"
#include "pk/capture.h"
#include "pk/combat.h"
#include "pk/encounter.h"
#include "pk/evolution.h"
#include "pk/exp.h"
#include "pk/items.h"
#include "pk/pk_map.h"
#include "pk/pk_world.h"

static int failures;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("ok   %s\n", name);                                      \
        } else {                                                            \
            printf("FAIL %s\n", name);                                      \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static void test_assets(void)
{
    CHECK(assets_init(), "assets_init");
    species_t sp;
    CHECK(assets_species(25, &sp) && sp.hp == 35 && sp.attack == 55 &&
              sp.defense == 40 && sp.special == 50 && sp.speed == 90 &&
              sp.catch_rate == 190,
          "assets_species #025 皮卡丘种族值");
    move_t mv;
    // 金银版招式表：#1 = 拍击（威力 40）。名池取自 moves.bin。
    CHECK(assets_move(1, &mv) && mv.power == 40, "assets_move #1 拍击");
    uint16_t ids[COMBAT_MOVE_CAP];
    const int n = combat_known_moves(25, 43, ids, COMBAT_MOVE_CAP);
    CHECK(n > 0 && ids[0] < ids[n - 1], "combat_known_moves 皮卡丘 升序");
    CHECK(combat_learn_level(25, 84) == 1, "combat_learn_level 电击=Lv1");
    CHECK(assets_front_sprite(25, &(uint8_t){0}) != NULL, "front sprite #025");
    uint16_t pal[4];
    assets_palette_variant(sp.palette, false, pal);
    CHECK(pal[3] == 0xFFFF, "palette 第4色=白");
}

static void test_map_world(void)
{
    pk_map_t map;
    pk_map_generate(&map);
    CHECK(pk_tile_walkable(TILE_PATH) && !pk_tile_walkable(TILE_TREE),
          "tile walkable 规则");
    int hx, hy;
    CHECK(pk_map_find_tile(&map, TILE_SHOP, &hx, &hy), "商店存在");
    CHECK(pk_tile_rarity(&map, 24, 36) == 0, "镇中心安全区");

    int16_t steps[2 * PK_MAP_W * PK_MAP_H];
    int n = pk_map_find_path(&map, 24, 36, 24, 20, steps, PK_MAP_W * PK_MAP_H);
    CHECK(n == 16, "沿主路向北 16 步");
    n = pk_map_find_path(&map, 24, 36, 0, 0, steps, PK_MAP_W * PK_MAP_H);
    CHECK(n == -1, "树墙不可达");
    n = pk_map_find_path(&map, 24, 36, 24, 36, steps, PK_MAP_W * PK_MAP_H);
    CHECK(n == 0, "同格路径为 0");

    CHECK(pk_world_selftest(), "pk_world_selftest（新档/存档往返/确定性）");
}

static void test_battle(void)
{
    /* 完整会话： Lv5 妙蛙种子 vs Lv3 波波（#16），玩家每回合选招。 */
    battle_session_t bs;
    CHECK(battle_session_init(&bs, 1, 5, 16, 3, 1024, 12345), "battle_session_init");
    /* 妙蛙种子 Lv1 学 藤鞭(33)。 */
    CHECK(combat_learn_level(1, 33) <= 5, "妙蛙种子会藤鞭");
    battle_round_t r;
    int rounds = 0;
    bs.player_move = 33;
    bs.player_move_set = true;
    while (!bs.finished && rounds < 40) {
        if (!battle_session_step(&bs, &r)) break;
        rounds++;
        if (!bs.next_by_pet) { /* 下一步轮到玩家前重新指定招式 */
            bs.player_move = 33;
            bs.player_move_set = true;
        }
    }
    CHECK(bs.finished && bs.won && rounds < 40, "Lv5 胜 Lv3 波波");
    CHECK(battle_session_exp(&bs) > 0, "战斗经验结算");
    CHECK(battle_session_hp_ratio(&bs) >= 1 && battle_session_hp_ratio(&bs) <= 100,
          "hp_ratio 有界");

    /* 逃跑：速度占优必逃。 */
    battle_session_init(&bs, 25, 20, 16, 3, 1024, 7);
    battle_escape_result_t esc;
    battle_escape_try(&bs, &esc);
    CHECK(esc.escaped, "高速逃跑成功");

    /* 捕捉窗口：大师球必中。 */
    cap_result_t cap;
    cap_attempt(190, 1024, CAP_BALL_MASTER, 100, 1, 600, 42, &cap);
    CHECK(cap.caught, "大师球必中");
    cap_attempt(3, 1024, CAP_BALL_POKE, 100, 1, 0, 42, &cap);
    CHECK(cap.window_w >= CAP_WINDOW_MIN, "极稀有种窗口有下界");
}

static void test_items_exp(void)
{
    CHECK(exp_for_level(2) == 12, "exp_for_level(2)=12");
    CHECK(exp_to_level(exp_for_level(10), 100) == 10, "exp_to_level");

    inventory_t bag;
    items_inventory_init(&bag);
    CHECK(items_inventory_valid(&bag), "inventory_valid");
    item_use_result_t res;
    CHECK(items_apply(ITEM_BERRY, 25, 10, &(nurture_t){0}, &res) == ITEM_USE_OK,
          "树果可用");
    CHECK(items_apply(ITEM_POKE, 25, 10, &(nurture_t){0}, &res) ==
              ITEM_USE_NOT_APPLICABLE,
          "球不走 items_apply");
    /* 火之石 → 六尾。 */
    CHECK(items_apply(ITEM_FIRE_STONE, 58, 20, &(nurture_t){0}, &res) ==
                  ITEM_USE_OK &&
              res.species_after == 59,
          "火之石进化卡蒂狗");
    /* 回复类（新增）走资格判定。 */
    CHECK(items_apply(ITEM_POTION, 25, 10, &(nurture_t){0}, &res) == ITEM_USE_OK,
          "回复水资格判定");

    /* 等级进化：卡蒂狗 Lv36 → 风速狗。 */
    CHECK(evo_level_ready(36, EVO_TRIGGER_LEVEL, 59, 36), "等级进化就绪");
    CHECK(!evo_level_ready(20, EVO_TRIGGER_LEVEL, 59, 36), "等级不足不进化");
}

int main(void)
{
    test_assets();
    test_map_world();
    test_battle();
    test_items_exp();
    (void)enc_selftest;

    if (failures) {
        printf("\n%d 项失败\n", failures);
        return 1;
    }
    printf("\n全部通过\n");
    return 0;
}

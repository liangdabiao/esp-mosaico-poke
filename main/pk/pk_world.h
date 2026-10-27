// pk/pk_world.h —— 游戏世界的唯一所有者（自原版 world.c 简化重写）。
//
// 原版 world.c 2000 行混了状态/扫描/事务/存档编排；这里按本仓工程规则
// 拆开：玩法状态只在这里，存盘走 pk_store，时间推进由 game.c 喂。
// 布局：单版本序列化（V1 + CRC），不做原版 V5~V17 迁移链。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "party.h"
#include "items.h"
#include "encounter.h"
#include "nurture.h"
#include "pk_map.h"

typedef struct {
    party_t party;
    inventory_t bag;
    dex_t dex;
    nurture_t nurture;      // 伙伴（队伍第一只）的三轴与亲密度
    uint32_t money;
    uint16_t px, py;        // 玩家格子坐标
    uint32_t steps;
    uint8_t has_starter;    // 0=未选初始伙伴
    uint32_t rng;           // 遇敌判定 LCG（存档带着走，序列可复现）
    uint16_t wins, catches;
} pk_world_t;

#define PK_MONEY_START 500
#define PK_MONEY_WIN(level) ((uint32_t)(level) * 12u + 30u)

void pk_world_init(pk_world_t *w);

// 开新档：初始伙伴 Lv5 + 5 精灵球 + 出生点。成功返回 true。
bool pk_world_new_game(pk_world_t *w, uint8_t starter_id, const pk_map_t *map);

mon_t *pk_world_leader(pk_world_t *w);

// 伙伴养成推进（喂真实时间，幂等）。game loop 每帧调用。
void pk_world_nurture_tick(pk_world_t *w, int64_t now_us);

// 踩到一格后判定是否遇敌。稀有种/等级/闪光全部确定（encounter.c），
// 只有「这一步遇不遇」用世界 LCG（存档携带，可复现）。
bool pk_world_roll_encounter(pk_world_t *w, const pk_map_t *map, int x, int y,
                             uint32_t ts, encounter_t *out);

// ---------------------------------------------------------------------------
// 存档序列化：单版本 V1。blob = "PKSV" | u8 ver | payload | u32 crc。
#define PK_SAVE_VERSION 1
#define PK_SAVE_BLOB_SIZE 2600

// 返回写入字节数；cap 不够返回 0。
uint32_t pk_world_serialize(const pk_world_t *w, uint8_t *buf, uint32_t cap);

// 校验 magic/版本/CRC。失败返回 false 且 w 不动。
bool pk_world_deserialize(pk_world_t *w, const uint8_t *buf, uint32_t len);

bool pk_world_selftest(void);

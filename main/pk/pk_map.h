// pk/pk_map.h —— 瓦片世界：程序生成 + 寻路 + 遭遇稀有度。
//
// 这是 Mosaico 版新增的层（原版没有地图，只有文字路线）。设计约束：
//   · **完全确定** —— 固定种子的 LCG 生成，宿主与设备逐格一致，
//     headless replay 才能对上。
//   · 稀有度按区域档位（1~5，0=安全区）驱动 enc_pick_species 的
//     物种池 —— 北部森林/东部山地出强种，镇子周围出常见种。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PK_MAP_W 48
#define PK_MAP_H 48

typedef enum {
    TILE_PATH = 0,      // 道路
    TILE_GRASS,         // 短草（低概率遇敌）
    TILE_TALL_GRASS,    // 深草（主遇敌点）
    TILE_TREE,          // 树（阻挡）
    TILE_WATER,         // 水（阻挡）
    TILE_ROCK,          // 岩石（阻挡）
    TILE_FLOWER,        // 花丛（装饰，可行走）
    TILE_SAND,          // 沙地（湖边）
    TILE_HOUSE,         // 民居（阻挡）
    TILE_SHOP,          // 商店门口（踏上去开商店）
    TILE_HEAL,          // 治疗之家门口（踏上去回复全队）
    TILE_FENCE,         // 栅栏（阻挡）
    TILE_COUNT,
} pk_tile_t;

typedef struct {
    uint8_t tiles[PK_MAP_W * PK_MAP_H];
    uint8_t zone[PK_MAP_W * PK_MAP_H];   // 遭遇稀有度档 0~5（0=不遇敌）
} pk_map_t;

// 固定种子生成。两端一致，不依赖时间或硬件。
void pk_map_generate(pk_map_t *m);

bool pk_tile_walkable(pk_tile_t t);

// 该格子的遭遇稀有度（0 = 不会遇敌）。
uint8_t pk_tile_rarity(const pk_map_t *m, int x, int y);

// 6 字节位置键（encounter 种子源）：[0]=1 地图 id，[1..2]=x，[3..4]=y，[5]=区档。
void pk_map_tile_key(const pk_map_t *m, int x, int y, uint8_t out[6]);

// BFS 寻路（等代价，最短步数）。out_steps 里存**中间与终点**格子坐标
// (x,y 交替，不含起点)。返回步数；放不下/不可达返回 -1。
int pk_map_find_path(const pk_map_t *m, int sx, int sy, int tx, int ty,
                     int16_t *out_steps, int max_steps);

// 特殊格子定位（生成保证存在；找不到返回 false）。
bool pk_map_find_tile(const pk_map_t *m, pk_tile_t t, int *x, int *y);

// pk/pk_map.c —— 瓦片世界生成与寻路。
//
// 布局（48×48，坐标 (0,0) 在左上）：
//   · 外圈一圈树墙，地图封闭。
//   · 中部偏南是常磐镇（路径网格 + 治疗之家 + 商店 + 民居/花丛），安全区。
//   · 镇子四周是平原草场（zone 2，短草/深草混点）。
//   · 北部森林：密树夹深草走廊（zone 3），最北一条稀有带（zone 4）。
//   · 西部大湖：水 + 沙滩环（zone 0/1，湖心岛沙滩 zone 3 够不着的彩蛋不做）。
//   · 东部山地：岩石阵 + 深草（zone 4），山顶稀有带（zone 5）。
//
// 生成只用固定种子的 LCG，无浮点、无时间 —— 宿主/设备/回放逐格一致。
#include <string.h>

#include "pk_map.h"

// 固定种子 LCG（LCG 参数取数值线性同余经典组）。
static uint32_t s_lcg;

static uint32_t lcg_next(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg;
}

static uint32_t lcg_below(uint32_t n)
{
    return n ? lcg_next() % n : 0;
}

static uint8_t *tile_at(pk_map_t *m, int x, int y)
{
    return &m->tiles[y * PK_MAP_W + x];
}

static uint8_t *zone_at(pk_map_t *m, int x, int y)
{
    return &m->zone[y * PK_MAP_W + x];
}

static void set(pk_map_t *m, int x, int y, pk_tile_t t, uint8_t zone)
{
    if (x < 0 || x >= PK_MAP_W || y < 0 || y >= PK_MAP_H) return;
    *tile_at(m, x, y) = (uint8_t)t;
    *zone_at(m, x, y) = zone;
}

// 实心矩形（含边界）。
static void fill(pk_map_t *m, int x0, int y0, int x1, int y1,
                 pk_tile_t t, uint8_t zone)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) set(m, x, y, t, zone);
    }
}

// 房子：3×2 屋身 + 门口（门口在屋身下沿中间）。
static void house(pk_map_t *m, int x, int y, pk_tile_t kind)
{
    fill(m, x, y, x + 2, y + 1, TILE_HOUSE, 0);
    set(m, x + 1, y + 2, kind, 0);   // 门口踏板
}

void pk_map_generate(pk_map_t *m)
{
    memset(m, 0, sizeof(*m));
    s_lcg = 0x504B474Fu;   // "PKGO" —— 固定种子，改了就换世界

    // 地面基底：平原短草，zone 2。
    fill(m, 0, 0, PK_MAP_W - 1, PK_MAP_H - 1, TILE_GRASS, 2);

    // 西部大湖（含沙滩环）。
    fill(m, 3, 6, 14, 20, TILE_WATER, 0);
    for (int y = 5; y <= 21; y++) {
        for (int x = 2; x <= 15; x++) {
            if (*tile_at(m, x, y) != TILE_GRASS) continue;
            // 湖外一圈草 → 沙（zone 1）
            bool near_water = false;
            for (int dy = -1; dy <= 1 && !near_water; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < PK_MAP_W && ny < PK_MAP_H &&
                        *tile_at(m, nx, ny) == TILE_WATER) {
                        near_water = true;
                        break;
                    }
                }
            }
            if (near_water) set(m, x, y, TILE_SAND, 1);
        }
    }

    // 北部森林（y 1..12）：树海 + 深草走廊；最北两行稀有带 zone 4。
    for (int y = 1; y <= 12; y++) {
        for (int x = 16; x < PK_MAP_W - 1; x++) {
            uint32_t r = lcg_below(100);
            if (y <= 2) {
                set(m, x, y, (r < 70) ? TILE_TALL_GRASS : TILE_TREE, 4);
            } else if (r < 45) {
                set(m, x, y, TILE_TREE, 0);
            } else if (r < 75) {
                set(m, x, y, TILE_TALL_GRASS, 3);
            } else {
                set(m, x, y, TILE_GRASS, 3);
            }
        }
    }

    // 东部山地（x 36..46, y 13..40）：岩阵 + 深草；最东一列稀有带 zone 5。
    for (int y = 13; y <= 40; y++) {
        for (int x = 36; x <= 46; x++) {
            uint32_t r = lcg_below(100);
            if (x >= 44) {
                set(m, x, y, (r < 60) ? TILE_TALL_GRASS : TILE_ROCK, 5);
            } else if (r < 35) {
                set(m, x, y, TILE_ROCK, 0);
            } else if (r < 70) {
                set(m, x, y, TILE_TALL_GRASS, 4);
            } else {
                set(m, x, y, TILE_GRASS, 4);
            }
        }
    }

    // 外圈树墙（最后压一遍，保证封闭）。
    for (int x = 0; x < PK_MAP_W; x++) {
        set(m, x, 0, TILE_TREE, 0);
        set(m, x, PK_MAP_H - 1, TILE_TREE, 0);
    }
    for (int y = 0; y < PK_MAP_H; y++) {
        set(m, 0, y, TILE_TREE, 0);
        set(m, PK_MAP_W - 1, y, TILE_TREE, 0);
    }

    // 常磐镇（中心偏南 x 17..31, y 28..40）：安全区 zone 0。
    fill(m, 17, 28, 31, 40, TILE_PATH, 0);
    // 镇子里两片花坛。
    fill(m, 18, 30, 20, 31, TILE_FLOWER, 0);
    fill(m, 28, 30, 30, 31, TILE_FLOWER, 0);

    // 主路：镇中心十字向外延伸到四方（先铺路，房子后放，避免被路打穿）。
    for (int y = 12; y <= 44; y++) set(m, 24, y, TILE_PATH, 0);
    for (int x = 3; x <= 44; x++) set(m, x, 36, TILE_PATH, 0);
    for (int x = 17; x <= 31; x++) set(m, x, 36, TILE_PATH, 0);

    // 建筑：治疗之家 / 商店 / 两栋民居。门口踏板是特殊格子。
    house(m, 19, 32, TILE_HEAL);
    house(m, 27, 32, TILE_SHOP);
    house(m, 20, 38, TILE_HOUSE);
    house(m, 27, 38, TILE_HOUSE);

    // 平原草场撒深草点（zone 2/3：靠森林、山地的一侧略好）。
    for (int y = 13; y <= 43; y++) {
        for (int x = 2; x <= 34; x++) {
            uint8_t t = *tile_at(m, x, y);
            if (t != TILE_GRASS || *zone_at(m, x, y) != 2) continue;
            if (lcg_below(100) < 22) set(m, x, y, TILE_TALL_GRASS, 3);
        }
    }

    // 出生点：镇中心（保证是路）。
    set(m, 24, 36, TILE_PATH, 0);
}

bool pk_tile_walkable(pk_tile_t t)
{
    switch (t) {
        case TILE_TREE: case TILE_WATER: case TILE_ROCK:
        case TILE_HOUSE: case TILE_FENCE:
            return false;
        default:
            return true;
    }
}

uint8_t pk_tile_rarity(const pk_map_t *m, int x, int y)
{
    if (x < 0 || x >= PK_MAP_W || y < 0 || y >= PK_MAP_H) return 0;
    return m->zone[y * PK_MAP_W + x];
}

void pk_map_tile_key(const pk_map_t *m, int x, int y, uint8_t out[6])
{
    out[0] = 1;   // 地图 id（当前只有一个世界）
    out[1] = (uint8_t)x;
    out[2] = (uint8_t)y;
    out[3] = 0;
    out[4] = 0;
    out[5] = pk_tile_rarity(m, x, y);
}

int pk_map_find_path(const pk_map_t *m, int sx, int sy, int tx, int ty,
                     int16_t *out_steps, int max_steps)
{
    if (!m || sx < 0 || sy < 0 || tx < 0 || ty < 0 ||
        sx >= PK_MAP_W || sy >= PK_MAP_H || tx >= PK_MAP_W || ty >= PK_MAP_H) {
        return -1;
    }
    if (sx == tx && sy == ty) return 0;
    if (!pk_tile_walkable((pk_tile_t)m->tiles[ty * PK_MAP_W + tx])) return -1;

    // BFS。48×48=2304 格，came_from 一字节方向码足够。
    static int16_t came[PK_MAP_W * PK_MAP_H];   // 前驱下标，-1=无
    static uint8_t visited[PK_MAP_W * PK_MAP_H];
    static int16_t queue[PK_MAP_W * PK_MAP_H];
    memset(visited, 0, sizeof(visited));
    for (int i = 0; i < PK_MAP_W * PK_MAP_H; i++) came[i] = -1;

    const int start = sy * PK_MAP_W + sx;
    const int goal = ty * PK_MAP_W + tx;
    int head = 0, tail = 0;
    queue[tail++] = (int16_t)start;
    visited[start] = 1;
    static const int DX[4] = {0, 0, -1, 1};
    static const int DY[4] = {-1, 1, 0, 0};

    bool found = false;
    while (head < tail) {
        int cur = queue[head++];
        if (cur == goal) { found = true; break; }
        int cx = cur % PK_MAP_W, cy = cur / PK_MAP_W;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= PK_MAP_W || ny >= PK_MAP_H) continue;
            int ni = ny * PK_MAP_W + nx;
            if (visited[ni]) continue;
            if (!pk_tile_walkable((pk_tile_t)m->tiles[ni])) continue;
            visited[ni] = 1;
            came[ni] = (int16_t)cur;
            queue[tail++] = (int16_t)ni;
        }
    }
    if (!found) return -1;

    // 回溯 → 反转。
    static int16_t rev[PK_MAP_W * PK_MAP_H];
    int n = 0;
    for (int cur = goal; cur != start && n < PK_MAP_W * PK_MAP_H; ) {
        rev[n++] = (int16_t)cur;
        cur = came[cur];
    }
    if (n > max_steps) return -1;
    for (int i = 0; i < n; i++) {
        out_steps[i * 2] = (int16_t)(rev[n - 1 - i] % PK_MAP_W);
        out_steps[i * 2 + 1] = (int16_t)(rev[n - 1 - i] / PK_MAP_W);
    }
    return n;
}

bool pk_map_find_tile(const pk_map_t *m, pk_tile_t t, int *x, int *y)
{
    for (int yy = 0; yy < PK_MAP_H; yy++) {
        for (int xx = 0; xx < PK_MAP_W; xx++) {
            if (m->tiles[yy * PK_MAP_W + xx] == (uint8_t)t) {
                if (x) *x = xx;
                if (y) *y = yy;
                return true;
            }
        }
    }
    return false;
}

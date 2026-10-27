// pk/pk_world.c —— 世界状态的实现（见 pk_world.h）。
#include <string.h>
#include <stdio.h>

#include "pk_world.h"
#include "battle.h"
#include "pk_port.h"

void pk_world_init(pk_world_t *w)
{
    if (!w) return;
    memset(w, 0, sizeof(*w));
    party_init(&w->party);
    items_inventory_init(&w->bag);
    dex_init(&w->dex);
    nurture_init(&w->nurture);
    w->money = PK_MONEY_START;
    w->rng = 0x3E6F776Eu;   // 固定初值；新档时再混淆一次
}

mon_t *pk_world_leader(pk_world_t *w)
{
    if (!w || !w->party.party_count) return NULL;
    return &w->party.party[0];
}

bool pk_world_new_game(pk_world_t *w, uint8_t starter_id, const pk_map_t *map)
{
    if (!w || !map || starter_id < 1 || starter_id > 151) return false;
    pk_world_init(w);

    mon_t starter = {0};
    starter.species_id = starter_id;
    starter.level = 5;
    starter.hp = combat_max_hp(starter_id, 5);
    starter.intimacy = 60;
    starter.exp = 0;
    if (!party_receive(&w->party, &starter)) return false;

    w->bag.quantity[ITEM_POKE] = 5;
    w->has_starter = 1;
    w->rng ^= pk_crc32(&starter_id, sizeof(starter_id)) | 1;
    w->px = 24;
    w->py = 36;
    if (!pk_tile_walkable((pk_tile_t)map->tiles[w->py * PK_MAP_W + w->px])) {
        int hx, hy;
        if (pk_map_find_tile(map, TILE_PATH, &hx, &hy)) {
            w->px = (uint16_t)hx;
            w->py = (uint16_t)hy;
        }
    }
    w->nurture.last_us = -1;   // 首次 tick 只记时刻不衰减
    return true;
}

void pk_world_nurture_tick(pk_world_t *w, int64_t now_us)
{
    if (!w) return;
    nurture_tick(&w->nurture, now_us, 0, false);
}

bool pk_world_roll_encounter(pk_world_t *w, const pk_map_t *map, int x, int y,
                             uint32_t ts, encounter_t *out)
{
    if (!w || !map || !out) return false;
    uint8_t rarity = pk_tile_rarity(map, x, y);
    if (!rarity) return false;

    pk_tile_t t = (pk_tile_t)map->tiles[y * PK_MAP_W + x];
    uint32_t denom = (t == TILE_TALL_GRASS) ? 6u : (t == TILE_GRASS) ? 18u : 0u;
    if (!denom) return false;

    w->rng = w->rng * 1664525u + 1013904223u;
    if (w->rng % denom != 0) return false;

    uint8_t key[6];
    pk_map_tile_key(map, x, y, key);
    const mon_t *leader = pk_world_leader(w);
    uint8_t level = battle_wild_level_for_pet(rarity, leader ? leader->level : 5);

    memset(out, 0, sizeof(*out));
    out->ts = ts;
    out->species_id = enc_pick_species(key, ts, rarity);
    out->rarity = rarity;
    out->level = level ? level : 2;
    out->hp_ratio = 100;
    out->is_shiny = enc_roll_shiny(key, ts, rarity);
    out->biome = 0;
    return true;
}

// ---------------------------------------------------------------------------
// 存档序列化（单版本 V1）
// ---------------------------------------------------------------------------

// 写入小工具：一次性把各字段排进 blob，指针推进由宏负责。
typedef struct {
    uint8_t *p;
    uint32_t left;
    bool ok;
} pk_writer_t;

static void put(pk_writer_t *wr, const void *src, uint32_t n)
{
    if (!wr->ok || wr->left < n) { wr->ok = false; return; }
    memcpy(wr->p, src, n);
    wr->p += n;
    wr->left -= n;
}

typedef struct {
    const uint8_t *p;
    uint32_t left;
    bool ok;
} pk_reader_t;

static bool get(pk_reader_t *rd, void *dst, uint32_t n)
{
    if (!rd->ok || rd->left < n) { rd->ok = false; return false; }
    memcpy(dst, rd->p, n);
    rd->p += n;
    rd->left -= n;
    return true;
}

#define FIELD(w, f) put((w), &(f), (uint32_t)sizeof(f))
#define TAKE(r, f) get((r), &(f), (uint32_t)sizeof(f))

uint32_t pk_world_serialize(const pk_world_t *w, uint8_t *buf, uint32_t cap)
{
    if (!w || !buf || cap < PK_SAVE_BLOB_SIZE) return 0;
    // 布局：[0..3]="PKSV" [4]=ver [5]=pad [6..]=payload [..+4]=crc。
    pk_writer_t wr = { buf, cap, true };

    put(&wr, "PKSV", 4);
    uint8_t ver = PK_SAVE_VERSION;
    put(&wr, &ver, 1);
    put(&wr, "\0", 1);

    FIELD(&wr, w->party);
    FIELD(&wr, w->bag);
    FIELD(&wr, w->dex);
    FIELD(&wr, w->nurture);
    FIELD(&wr, w->money);
    FIELD(&wr, w->px);
    FIELD(&wr, w->py);
    FIELD(&wr, w->steps);
    FIELD(&wr, w->has_starter);
    FIELD(&wr, w->rng);
    FIELD(&wr, w->wins);
    FIELD(&wr, w->catches);
    if (!wr.ok) return 0;

    const uint32_t payload = (uint32_t)(wr.p - buf);
    const uint32_t crc = pk_crc32(buf, payload);
    memcpy(wr.p, &crc, sizeof(crc));
    return payload + (uint32_t)sizeof(crc);
}

bool pk_world_deserialize(pk_world_t *w, const uint8_t *buf, uint32_t len)
{
    if (!w || !buf || len < 6 + 4) return false;
    if (memcmp(buf, "PKSV", 4) != 0) return false;
    if (buf[4] != PK_SAVE_VERSION) return false;

    // len = 6 字节头 + 字段区 + 4 字节 CRC；CRC 覆盖整个头部+字段区。
    const uint32_t fields = len - 4;
    uint32_t crc_stored, crc_calc = pk_crc32(buf, fields);
    memcpy(&crc_stored, buf + fields, sizeof(crc_stored));
    if (crc_stored != crc_calc) return false;

    // 校验全部通过才落进 w —— 失败时 w 不动。
    pk_world_t tmp;
    pk_reader_t rd = { buf + 6, fields - 6, true };
    if (!TAKE(&rd, tmp.party) || !TAKE(&rd, tmp.bag) || !TAKE(&rd, tmp.dex) ||
        !TAKE(&rd, tmp.nurture) || !TAKE(&rd, tmp.money) || !TAKE(&rd, tmp.px) ||
        !TAKE(&rd, tmp.py) || !TAKE(&rd, tmp.steps) || !TAKE(&rd, tmp.has_starter) ||
        !TAKE(&rd, tmp.rng) || !TAKE(&rd, tmp.wins) || !TAKE(&rd, tmp.catches) ||
        !rd.ok || rd.left != 0) {
        return false;
    }
    if (!items_inventory_valid(&tmp.bag) || tmp.party.party_count > PARTY_MAX) {
        return false;
    }
    *w = tmp;
    return true;
}

bool pk_world_selftest(void)
{
    bool ok = true;
    pk_map_t map;
    pk_map_generate(&map);

    pk_world_t w;
    pk_world_init(&w);
    if (!pk_world_new_game(&w, 1, &map)) {
        printf("world: 开新档失败\n");
        return false;
    }
    if (!w.has_starter || w.party.party_count != 1 ||
        w.bag.quantity[ITEM_POKE] != 5 || w.money != PK_MONEY_START) {
        printf("world: 新档初始状态不对\n");
        ok = false;
    }

    // 序列化往返
    uint8_t blob[PK_SAVE_BLOB_SIZE];
    uint32_t n = pk_world_serialize(&w, blob, sizeof(blob));
    if (!n) {
        printf("world: 序列化失败\n");
        return false;
    }
    pk_world_t back;
    pk_world_init(&back);
    if (!pk_world_deserialize(&back, blob, n)) {
        printf("world: 反序列化失败\n");
        return false;
    }
    if (back.money != w.money || back.px != w.px ||
        memcmp(&back.party, &w.party, sizeof(party_t)) != 0 ||
        memcmp(&back.dex, &w.dex, sizeof(dex_t)) != 0) {
        printf("world: 往返不一致\n");
        ok = false;
    }

    // 坏数据必须被拒：改一个字节（CRC 之后）
    blob[n - 1] ^= 0xFF;
    if (pk_world_deserialize(&back, blob, n)) {
        printf("world: CRC 损坏没被挡住\n");
        ok = false;
    }

    // 遇敌确定性：同 rng、同格子 → 同结果序列
    pk_world_t a = w, b = w;
    int hit = 0;
    for (int i = 0; i < 200; i++) {
        encounter_t ea, eb;
        bool ra = pk_world_roll_encounter(&a, &map, 8, 8, 100, &ea);
        bool rb = pk_world_roll_encounter(&b, &map, 8, 8, 100, &eb);
        if (ra != rb) { printf("world: 遇敌判定不确定\n"); ok = false; break; }
        if (ra) {
            hit++;
            if (ea.species_id != eb.species_id || ea.is_shiny != eb.is_shiny) {
                printf("world: 遇敌内容不确定\n");
                ok = false;
            }
            break;
        }
    }
    (void)hit;

    // 安全区永不遇敌
    encounter_t e;
    if (pk_world_roll_encounter(&a, &map, 24, 36, 100, &e)) {
        printf("world: 安全区（镇中）不应遇敌\n");
        ok = false;
    }

    if (ok) printf("world: 自检 全部通过（新档 · 往返 · CRC · 确定性 · 安全区）\n");
    return ok;
}

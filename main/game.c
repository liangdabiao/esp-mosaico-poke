// SPDX-License-Identifier: Apache-2.0
// game.c —— PokeGo for Mosaico：触摸版主逻辑（自 ESP32-PokemonGo 移植 + 重交互）。
//
// 玩法引擎全部来自原版移植（pk/）：属性相克战斗、时机条捕捉、
// 确定性遭遇、图鉴/队伍/背包/进化/养成。本文件只做两件事：
//   · 页面状态机与触摸交互（原版是三键光标，这里全部重做为直触）
//   · 480×480 方屏的排版（底部五标签导航 + 上方内容区）
//
// 版面约定：底部 64px 是常驻标签栏（战斗/捕捉页除外），上方 416px 是
// 内容区。地图 48px 一格，摄像头跟随玩家。所有手势命中使用的几何都是
// 常量或 update 路径状态，render 只读 —— 第一次触摸不会读到初值。
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "game_config.h"
#include "mosaico_raylib_fast.h"

#include "pk/assets.h"
#include "pk/battle.h"
#include "pk/battle_escape.h"
#include "pk/capture.h"
#include "pk/combat.h"
#include "pk/exp.h"
#include "pk/evolution.h"
#include "pk/items.h"
#include "pk/nurture.h"
#include "pk/party.h"
#include "pk/pk_draw.h"
#include "pk/pk_map.h"
#include "pk/pk_port.h"
#include "pk/pk_store.h"
#include "pk/pk_world.h"
#include "pkaudio/wq_sfx.h"
#include "pkaudio/wq_volume.h"

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

#define VIEW_H (GAME_HEIGHT - 64)   /* 内容区高度 */
#define NAV_H  64

#define TILE_PX 48                  /* 地图一格的屏幕像素 */
#define WALK_TICKS 4                /* 每 N tick 走一格（30Hz） */

#define BP_MSG_H 116                /* 战斗页底部消息/菜单条 */
#define BP_OVERLAY_H 200            /* 战斗子菜单覆盖板高度 */

#define CAP_BAR_X 40
#define CAP_BAR_W 400               /* 判定条逻辑宽 200，屏上 ×2 画 */

#define DEX_COLS 5
#define DEX_CELL 88
#define DEX_ROWS_VISIBLE 4

/* 起始三伙伴：妙蛙种子 / 小火龙 / 杰尼龟。 */
static const uint16_t STARTERS[3] = {1, 4, 7};

/* 交易所在售商品。 */
typedef struct {
    uint8_t item;
    uint32_t price;
} shop_entry_t;
static const shop_entry_t SHOP_STOCK[] = {
    { ITEM_POKE, 200 },
    { ITEM_GREAT, 600 },
    { ITEM_ULTRA, 1200 },
    { ITEM_POTION, 300 },
    { ITEM_SUPER_POTION, 700 },
    { ITEM_MAX_POTION, 1500 },
    { ITEM_BERRY, 100 },
    { ITEM_ENERGY_ROOT, 250 },
    { ITEM_MILK, 500 },
    { ITEM_JOY_COOKIE, 800 },
};
#define SHOP_STOCK_COUNT (int)(sizeof(SHOP_STOCK) / sizeof(SHOP_STOCK[0]))

/* 页面。 */
typedef enum {
    PAGE_STARTER = 0,
    PAGE_MAP,
    PAGE_PARTY,
    PAGE_PARTY_DETAIL,
    PAGE_BAG,
    PAGE_BAG_TARGET,
    PAGE_DEX,
    PAGE_DEX_DETAIL,
    PAGE_SHOP,
    PAGE_CARE,
    PAGE_MORE,
    PAGE_BATTLE,
    PAGE_CAPTURE,
} page_t;

/* 底部标签。 */
typedef enum {
    TAB_MAP = 0, TAB_DEX, TAB_PARTY, TAB_BAG, TAB_MORE, TAB_COUNT
} tab_t;

/* 战斗页相位。 */
typedef enum {
    BP_INTRO = 0,   /* 遭遇消息，点按进入菜单 */
    BP_MENU,        /* 攻击/背包/队伍/逃跑 */
    BP_MOVES,       /* 招式网格 */
    BP_MSG,         /* 回合消息，点按继续（队列） */
    BP_BAG,         /* 战斗用包 */
    BP_SWITCH,      /* 换宠 */
    BP_END,         /* 结束，点按回地图 */
} battle_phase_t;

/* 捕捉页相位。 */
typedef enum {
    CP_AIM = 0,
    CP_RESULT,
} capture_phase_t;

/* 音量档位（wq_volume 档位表管理，游戏层只做循环与下发）。 */
uint8_t game_volume_level(void);
void game_volume_cycle(void);

/* 页面。 */
struct game_t {
    game_config_t config;
    game_snapshot_t state;

    pk_map_t map;
    pk_world_t world;
    page_t page;
    tab_t tab;

    /* 全局消息浮层（地图页非阻断提示）。 */
    char msg[128];
    int msg_ticks;

    /* 时间：固定 30Hz，tick 推导 —— 回放确定。 */
    uint32_t ts;

    /* 地图行走。 */
    int16_t path[PK_MAP_W * PK_MAP_H];   /* x,y 交替 */
    int path_len, path_idx;
    int walk_phase;
    uint16_t dest_x, dest_y;             /* 0xFFFF = 无 */
    uint8_t facing;                      /* 0上1下2左3右 */

    /* 各页游标。 */
    int starter_sel;
    int dex_top;
    int dex_drag_y, dex_drag_anchor;
    bool dex_dragging;
    bool dex_moved;                      /* 本次手势是否拖动过 */
    int dex_sel;
    int party_sel;
    int bag_sel;
    uint8_t bag_pending_item;
    int shop_sel;

    /* 战斗。 */
    battle_session_t bs;
    battle_phase_t bp;
    uint16_t battle_moves[8];
    int battle_move_count;
    char battle_msg[128];
    battle_round_t battle_pending[4];    /* 待展示的半回合 */
    int battle_pending_n;
    battle_round_t battle_last;
    uint8_t wild_rarity;
    uint16_t wild_species;
    uint8_t wild_level;
    bool wild_shiny;
    bool battle_won;
    uint8_t loot_item, loot_qty;
    bool loot_pending;

    /* 捕捉。 */
    capture_phase_t cp;
    int cap_start_tick;
    cap_result_t cap_res;
    uint8_t cap_ball;

    /* 进化演出。 */
    struct {
        exp_growth_queue_t q;
        bool active;
        uint8_t from, to, slot;
        int tick;
        bool done;
    } evo;
};

/* ---- 颜色 ---- */
static const Color C_SKY      = { 168, 214, 235, 255 };
static const Color C_PANEL    = { 246, 240, 222, 255 };
static const Color C_PANEL_DK = { 226, 216, 192, 255 };
static const Color C_TEXT     = { 45, 36, 24, 255 };
static const Color C_TEXT_INV = { 250, 248, 240, 255 };
static const Color C_ACCENT   = { 92, 148, 88, 255 };
static const Color C_DANGER   = { 196, 84, 72, 255 };
static const Color C_DARK     = { 52, 48, 44, 255 };
static const Color C_NAV      = { 62, 58, 54, 255 };
static const Color C_NAV_SEL  = { 96, 128, 88, 255 };
static const Color C_BAR_BG   = { 40, 40, 44, 220 };

/* ---- 地图配色 ---- */
static Color tile_color(pk_tile_t t)
{
    switch (t) {
        case TILE_PATH:       return (Color){ 214, 190, 148, 255 };
        case TILE_GRASS:      return (Color){ 140, 200, 110, 255 };
        case TILE_TALL_GRASS: return (Color){ 84, 156, 82, 255 };
        case TILE_TREE:       return (Color){ 44, 102, 58, 255 };
        case TILE_WATER:      return (Color){ 84, 148, 214, 255 };
        case TILE_ROCK:       return (Color){ 138, 132, 126, 255 };
        case TILE_FLOWER:     return (Color){ 226, 168, 196, 255 };
        case TILE_SAND:       return (Color){ 232, 214, 160, 255 };
        case TILE_HOUSE:      return (Color){ 196, 120, 84, 255 };
        case TILE_SHOP:       return (Color){ 226, 178, 70, 255 };
        case TILE_HEAL:       return (Color){ 214, 96, 96, 255 };
        case TILE_FENCE:      return (Color){ 150, 116, 82, 255 };
        default:              return (Color){ 140, 200, 110, 255 };
    }
}

/* ---------------------------------------------------------------------------
 * 小工具
 * --------------------------------------------------------------------------- */

static const char *rarity_name(uint8_t r)
{
    static const char *N[6] = { "", "常见", "常见", "稀有", "稀有", "极稀有" };
    return (r >= 1 && r <= 5) ? N[r] : "常见";
}

static const char *type_name(uint8_t ty)
{
    static const char *N[17] = {
        "普通", "火", "水", "电", "草", "冰", "格斗", "毒", "地面",
        "飞行", "超能", "虫", "岩石", "幽灵", "龙", "恶", "钢",
    };
    return ty < 17 ? N[ty] : "普通";
}

static void show_msg(game_handle_t g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->msg, sizeof(g->msg), fmt, ap);
    va_end(ap);
    g->msg_ticks = 150;
}

/* 物种名安全拷贝（名字池非 NUL 结尾）。 */
static void species_name(uint16_t id, char *out, size_t cap)
{
    species_t sp;
    if (cap == 0) return;
    out[0] = '\0';
    if (assets_species(id, &sp)) {
        size_t n = (size_t)sp.name_zh_len;
        if (n >= cap) n = cap - 1;
        memcpy(out, sp.name_zh, n);
        out[n] = '\0';
    }
}

static void save_game(game_handle_t g)
{
    uint8_t blob[PK_SAVE_BLOB_SIZE];
    uint32_t n = pk_world_serialize(&g->world, blob, sizeof(blob));
    if (n) (void)pk_store_save(blob, n);
}

static void heal_party_all(pk_world_t *w)
{
    for (int i = 0; i < w->party.party_count; i++) {
        mon_t *m = &w->party.party[i];
        m->hp = combat_max_hp(m->species_id, m->level);
    }
}

static uint16_t leader_ability_q10(pk_world_t *w)
{
    nurt_mood_t mood = nurture_mood(&w->nurture);
    return (mood == NURT_MOOD_DESPONDENT) ? 614u : 1024u;
}

/* 画一只 2bpp 精灵（含闪光调色板与呼吸浮动）。 */
static void draw_mon(game_handle_t g, uint16_t species, bool shiny, bool back,
                     int cx, int cy, int scale, bool bob)
{
    const uint8_t *data;
    int w, h;
    uint16_t pal[4];
    species_t sp;
    if (!assets_species(species, &sp)) return;
    assets_palette_variant(sp.palette, shiny, pal);
    if (back) {
        sprite_asset_t asset;
        if (!assets_back_sprite_info(species, &asset)) return;
        data = asset.data;
        w = asset.w;
        h = asset.h;
    } else {
        uint8_t size;
        data = assets_front_sprite(species, &size);
        if (!data) return;
        w = h = size;
    }
    const int px = cx - w * scale / 2;
    const int py = cy - h * scale / 2 + (bob ? (int)((g->state.tick / 20) % 2) : 0);
    pk_draw_sprite_2bpp(px, py, data, w, h, scale, pal);
}

/* ---------------------------------------------------------------------------
 * 导航 / 页框
 * --------------------------------------------------------------------------- */

static pk_rect_t nav_rect(tab_t t)
{
    return (pk_rect_t){ t * 96, GAME_HEIGHT - NAV_H, 96, NAV_H };
}

static const char *nav_label(tab_t t)
{
    static const char *L[TAB_COUNT] = { "地图", "图鉴", "队伍", "背包", "我的" };
    return L[t];
}

/* 标签栏点击：返回 true 表示已切换页面。 */
static bool nav_tap(game_handle_t g, int x, int y)
{
    if (y < VIEW_H) return false;
    for (int t = 0; t < TAB_COUNT; t++) {
        pk_rect_t r = nav_rect((tab_t)t);
        if (!pk_hit(&r, x, y)) continue;
        g->tab = (tab_t)t;
        g->page = (t == TAB_MAP) ? PAGE_MAP
                : (t == TAB_DEX) ? PAGE_DEX
                : (t == TAB_PARTY) ? PAGE_PARTY
                : (t == TAB_BAG) ? PAGE_BAG : PAGE_MORE;
        g->dex_dragging = false;
        wq_sfx_play(WQ_TONE_MOVE);
        return true;
    }
    return false;
}

static void draw_nav(game_handle_t g)
{
    DrawRectangle(0, VIEW_H, GAME_WIDTH, NAV_H, C_NAV);
    for (int t = 0; t < TAB_COUNT; t++) {
        pk_rect_t r = nav_rect((tab_t)t);
        const bool sel = (g->tab == (tab_t)t);
        if (sel) DrawRectangle(r.x, r.y, r.w, r.h, C_NAV_SEL);
        const char *label = nav_label((tab_t)t);
        pk_draw_text(r.x + (r.w - pk_text_width(label)) / 2,
                     r.y + (r.h - pk_font_size()) / 2, label, C_TEXT_INV);
    }
    DrawLine(0, VIEW_H, GAME_WIDTH, VIEW_H, C_DARK);
}

static void draw_page_title_at(const char *title, int x)
{
    DrawRectangle(0, 0, GAME_WIDTH, 44, C_PANEL_DK);
    pk_draw_text(x, 14, title, C_TEXT);
    DrawLine(0, 44, GAME_WIDTH, 44, C_DARK);
}

static void draw_page_title(const char *title)
{
    draw_page_title_at(title, 16);
}

static void draw_back_btn(void)
{
    DrawRectangle(0, 0, 72, 44, C_DARK);
    /* 左箭头用三角形画 —— 宿主默认字体里 '<' 渲染不可靠。 */
    DrawTriangle((Vector2){ 44, 10 }, (Vector2){ 44, 34 }, (Vector2){ 26, 22 },
                 C_TEXT_INV);
}

static bool back_hit(int x, int y)
{
    return x < 72 && y < 44;
}

/* ---------------------------------------------------------------------------
 * 战斗编排
 * --------------------------------------------------------------------------- */

static void battle_reload_moves(game_handle_t g)
{
    mon_t *leader = pk_world_leader(&g->world);
    g->battle_move_count = 0;
    if (!leader) return;
    uint16_t ids[COMBAT_MOVE_CAP];
    const int n = combat_known_moves(leader->species_id, leader->level, ids,
                                     COMBAT_MOVE_CAP);
    /* 展示最高 ID 的 8 个（升级学习表按等级递增，越靠后越强）。 */
    const int start = n > 8 ? n - 8 : 0;
    for (int i = start; i < n && g->battle_move_count < 8; i++) {
        g->battle_moves[g->battle_move_count++] = ids[i];
    }
}

static void battle_begin(game_handle_t g, const encounter_t *enc)
{
    pk_world_t *w = &g->world;
    g->wild_species = enc->species_id;
    g->wild_level = enc->level;
    g->wild_shiny = enc->is_shiny;
    g->wild_rarity = enc->rarity;
    mon_t *leader = pk_world_leader(w);

    memset(&g->bs, 0, sizeof(g->bs));
    uint32_t seed = w->rng;
    w->rng = w->rng * 1664525u + 1013904223u;
    if (!leader || !battle_session_init(&g->bs, leader->species_id, leader->level,
                                        enc->species_id, enc->level,
                                        leader_ability_q10(w), seed)) {
        show_msg(g, "队伍没有能战斗的伙伴");
        g->page = PAGE_MAP;
        return;
    }
    char wild_name[24];
    species_name(enc->species_id, wild_name, sizeof(wild_name));
    snprintf(g->battle_msg, sizeof(g->battle_msg), "野生的%s出现了！",
             wild_name);
    g->bp = BP_INTRO;
    g->battle_move_count = 0;
    g->battle_pending_n = 0;
    g->loot_pending = false;
    g->page = PAGE_BATTLE;
    wq_sfx_play(WQ_TONE_ENTER);
}

/* 把一个半回合转成展示文本。 */
static void battle_round_text(game_handle_t g, const battle_round_t *r,
                              char *out, size_t cap)
{
    char src_name[24];
    const uint16_t src_id = r->by_pet ? g->bs.pet_species : g->bs.wild_species;
    species_name(src_id, src_name, sizeof(src_name));
    if (r->move_id && r->move_zh_len) {
        char mv[32];
        snprintf(mv, sizeof(mv), "%.*s", r->move_zh_len, r->move_zh);
        if (r->by_pet) {
            snprintf(out, cap, "%s使出了%s！", src_name, mv);
        } else {
            snprintf(out, cap, "对手使出了%s！", mv);
        }
        const char *fx = combat_feedback(r);
        if (fx && fx[0]) {
            size_t len = strlen(out);
            snprintf(out + len, cap - len, "%s", fx);
        }
        if (r->damage) {
            size_t len = strlen(out);
            snprintf(out + len, cap - len, " -%u", r->damage);
        }
    } else {
        const char *fx = combat_feedback(r);
        snprintf(out, cap, "%s", (fx && fx[0]) ? fx : "...");
    }
}

/* 推进一个半回合并入队展示。返回 false 表示战斗已结束。 */
static bool battle_step_push(game_handle_t g)
{
    if (g->bs.finished) return false;
    if (g->battle_pending_n >= (int)(sizeof(g->battle_pending) /
                                     sizeof(g->battle_pending[0]))) {
        return true;   /* 队列满：等玩家点完再说 */
    }
    battle_round_t r;
    if (!battle_session_step(&g->bs, &r)) return false;
    g->battle_last = r;
    g->battle_pending[g->battle_pending_n++] = r;
    wq_sfx_play(r.damage ? WQ_TONE_PLACE : WQ_TONE_MOVE);
    return true;
}

/* 战斗结束时的结算。 */
static void battle_finish(game_handle_t g)
{
    g->battle_won = g->bs.won;
    g->bp = BP_END;

    pk_world_t *w = &g->world;
    if (g->battle_won) {
        w->wins++;
        const uint16_t exp = battle_session_exp(&g->bs);
        uint32_t money = PK_MONEY_WIN(g->wild_level);
        if (money > 999) money = 999;
        w->money += money;

        mon_t before[PARTY_MAX];
        const unsigned count = w->party.party_count;
        memcpy(before, w->party.party, sizeof(before));
        const unsigned eligible = (1u << count) - 1u;
        exp_award_party(&w->party, 1u, eligible, exp);
        mon_t *leader = pk_world_leader(w);
        if (leader && leader->explore_value < 999) leader->explore_value++;
        exp_growth_record(&g->evo.q, before, count, w->party.party, count);

        const uint32_t loot_seed = items_loot_seed(g->bs.rng, w->steps,
                                                   g->wild_species, g->wild_rarity);
        const item_loot_t loot = items_roll_loot(g->wild_rarity, loot_seed);
        if (loot.item_id != ITEM_NONE && loot.quantity) {
            uint16_t cap = items_capacity(loot.item_id);
            uint16_t space = (uint16_t)(cap - w->bag.quantity[loot.item_id]);
            uint16_t add = loot.quantity > space ? space : loot.quantity;
            if (add) {
                w->bag.quantity[loot.item_id] =
                    (uint16_t)(w->bag.quantity[loot.item_id] + add);
                g->loot_item = loot.item_id;
                g->loot_qty = add;
                g->loot_pending = true;
            }
        }
    } else {
        /* 战败：全队送回回复之家，伙伴心情受挫。 */
        nurture_defeat(&w->nurture);
        int hx, hy;
        if (pk_map_find_tile(&g->map, TILE_HEAL, &hx, &hy)) {
            w->px = (uint16_t)hx;
            w->py = (uint16_t)hy;
        }
        heal_party_all(w);
        g->dest_x = 0xFFFF;
    }
    save_game(g);
}

/* 点按消息推进：出队一条；队列空则回菜单或结算。 */
static void battle_advance_msg(game_handle_t g)
{
    if (g->battle_pending_n > 0) {
        const battle_round_t r = g->battle_pending[0];
        g->battle_pending_n--;
        memmove(g->battle_pending, g->battle_pending + 1,
                (size_t)g->battle_pending_n * sizeof(g->battle_pending[0]));
        battle_round_text(g, &r, g->battle_msg, sizeof(g->battle_msg));
        g->bp = BP_MSG;
        return;
    }
    if (g->bs.finished) {
        battle_finish(g);
        char buf[96];
        if (g->battle_won) {
            if (g->loot_pending) {
                const item_info_t *info = items_info(g->loot_item);
                snprintf(buf, sizeof(buf), "获得了%sx%u！",
                         info ? info->name : "?", g->loot_qty);
                g->loot_pending = false;
            } else {
                snprintf(buf, sizeof(buf), "战斗胜利！");
            }
        } else {
            snprintf(buf, sizeof(buf), "战败... 全队被送回了回复之家");
        }
        snprintf(g->battle_msg, sizeof(g->battle_msg), "%s", buf);
        g->bp = BP_END;
        return;
    }
    g->bp = BP_MENU;
    battle_reload_moves(g);
}

/* 演完「我方行动 + 对手回应」，把消息排进队列。 */
static void battle_run_round(game_handle_t g)
{
    /* 玩家半回合 */
    if (!battle_step_push(g)) return;
    /* 对手半回合（若轮到） */
    if (!g->bs.finished && !g->bs.next_by_pet) battle_step_push(g);
    if (g->bs.finished) {
        /* 直接结算：清队列展示最后一条再进 END。 */
        battle_finish(g);
        g->battle_pending_n = 0;
        char buf[96];
        if (g->battle_won) {
            snprintf(buf, sizeof(buf), "战斗胜利！");
            if (g->loot_pending) {
                const item_info_t *info = items_info(g->loot_item);
                size_t len = strlen(buf);
                snprintf(buf + len, sizeof(buf) - len, " 获得了%sx%u！",
                         info ? info->name : "?", g->loot_qty);
                g->loot_pending = false;
            }
        } else {
            snprintf(buf, sizeof(buf), "战败... 被送回了回复之家");
        }
        snprintf(g->battle_msg, sizeof(g->battle_msg), "%s", buf);
        g->bp = BP_END;
        return;
    }
    /* 弹出第一条展示，其余排队等点按。 */
    const battle_round_t r = g->battle_pending[0];
    g->battle_pending_n--;
    memmove(g->battle_pending, g->battle_pending + 1,
            (size_t)g->battle_pending_n * sizeof(g->battle_pending[0]));
    battle_round_text(g, &r, g->battle_msg, sizeof(g->battle_msg));
    g->bp = BP_MSG;
}

/* 升级/进化事件推进。有进化时进入演出（战斗/道具共用）。 */
static void pump_growth(game_handle_t g)
{
    if (g->evo.active) return;
    exp_growth_t ev;
    while (exp_growth_pop(&g->evo.q, &ev)) {
        species_t sp;
        if (!assets_species(ev.species, &sp)) continue;
        if (sp.evolve_trigger == EVO_TRIGGER_LEVEL &&
            evo_level_ready(ev.after, sp.evolve_trigger, sp.evolve_to,
                            sp.evolve_level)) {
            g->evo.active = true;
            g->evo.from = (uint8_t)ev.species;
            g->evo.to = (uint8_t)sp.evolve_to;
            g->evo.slot = ev.slot;
            g->evo.tick = 0;
            g->evo.done = false;
            wq_sfx_play(WQ_TONE_COMPLETE);
            return;
        }
    }
}

static void apply_evolution(game_handle_t g)
{
    pk_world_t *w = &g->world;
    mon_t *m = (g->evo.slot < w->party.party_count)
                   ? &w->party.party[g->evo.slot] : pk_world_leader(w);
    g->evo.active = false;
    if (!m) return;
    species_t sp;
    if (assets_species(g->evo.to, &sp)) {
        dex_mark_seen(&w->dex, m->species_id, false);
        m->species_id = g->evo.to;
        m->hp = combat_max_hp(m->species_id, m->level);
        dex_mark_caught(&w->dex, g->evo.to, false);
        save_game(g);
    }
}

/* ---------------------------------------------------------------------------
 * STARTER
 * --------------------------------------------------------------------------- */

static void render_starter(game_handle_t g)
{
    ClearBackground(C_SKY);
    pk_draw_text_scaled((GAME_WIDTH - pk_text_width_scaled("选择你的伙伴", 2)) / 2,
                        40, "选择你的伙伴", C_TEXT, 2);
    pk_draw_text((GAME_WIDTH - pk_text_width("旅程从这一只开始")) / 2, 90,
                 "旅程从这一只开始", C_TEXT);
    for (int i = 0; i < 3; i++) {
        pk_rect_t card = { 40 + i * 140, 140, 120, 190 };
        const bool sel = (g->starter_sel == i);
        DrawRectangle(card.x, card.y, card.w, card.h,
                      sel ? (Color){ 214, 236, 196, 255 } : C_PANEL);
        DrawRectangleLines(card.x, card.y, card.w, card.h, C_DARK);
        char name[24];
        species_name(STARTERS[i], name, sizeof(name));
        pk_draw_text(card.x + (card.w - pk_text_width(name)) / 2, card.y + 14,
                     name, C_TEXT);
        draw_mon(g, STARTERS[i], false, false, card.x + card.w / 2,
                 card.y + 100, 2, true);
        DrawText("Lv5", card.x + card.w / 2 - 16, card.y + card.h - 26, 18, C_TEXT);
    }
    pk_draw_text((GAME_WIDTH - pk_text_width("点按选择，再点一次确认")) / 2, 366,
                 "点按选择，再点一次确认", C_TEXT);
}

static void tap_starter(game_handle_t g, int x, int y)
{
    for (int i = 0; i < 3; i++) {
        pk_rect_t card = { 40 + i * 140, 140, 120, 190 };
        if (!pk_hit(&card, x, y)) continue;
        if (g->starter_sel == i) {
            if (pk_world_new_game(&g->world, (uint8_t)STARTERS[i], &g->map)) {
                g->page = PAGE_MAP;
                g->tab = TAB_MAP;
                save_game(g);
                wq_sfx_play(WQ_TONE_CORRECT);
                show_msg(g, "旅程开始了！");
            } else {
                show_msg(g, "开档失败");
            }
        } else {
            g->starter_sel = i;
            wq_sfx_play(WQ_TONE_MOVE);
        }
        return;
    }
}

/* ---------------------------------------------------------------------------
 * MAP
 * --------------------------------------------------------------------------- */

static void map_camera(const game_handle_t g, int *cam_x, int *cam_y)
{
    *cam_x = (int)g->world.px * TILE_PX + TILE_PX / 2 - GAME_WIDTH / 2;
    *cam_y = (int)g->world.py * TILE_PX + TILE_PX / 2 - VIEW_H / 2;
    const int world_w = PK_MAP_W * TILE_PX;
    const int world_h = PK_MAP_H * TILE_PX;
    if (*cam_x < 0) *cam_x = 0;
    if (*cam_y < 0) *cam_y = 0;
    if (*cam_x > world_w - GAME_WIDTH) *cam_x = world_w - GAME_WIDTH;
    if (*cam_y > world_h - VIEW_H) *cam_y = world_h - VIEW_H;
}

static void draw_map_hud(game_handle_t g)
{
    DrawRectangle(0, 0, GAME_WIDTH, 46, C_BAR_BG);
    char buf[48];
    snprintf(buf, sizeof(buf), "$%lu", (unsigned long)g->world.money);
    pk_draw_text(12, 8, buf, C_TEXT_INV);
    snprintf(buf, sizeof(buf), "图鉴 %u", dex_count_caught(&g->world.dex));
    pk_draw_text(140, 8, buf, C_TEXT_INV);
    mon_t *leader = pk_world_leader(&g->world);
    if (leader) {
        char name[24];
        species_name(leader->species_id, name, sizeof(name));
        snprintf(buf, sizeof(buf), "%s Lv%u", name, leader->level);
        pk_draw_text(230, 8, buf, C_TEXT_INV);
        pk_draw_hp_bar(230, 30, 180, 10, leader->hp,
                       combat_max_hp(leader->species_id, leader->level));
    }
}

static void render_map(game_handle_t g)
{
    ClearBackground(C_SKY);
    int cam_x, cam_y;
    map_camera(g, &cam_x, &cam_y);

    const int x0 = cam_x / TILE_PX, y0 = cam_y / TILE_PX;
    const int x1 = (cam_x + GAME_WIDTH) / TILE_PX + 1;
    const int y1 = (cam_y + VIEW_H) / TILE_PX + 1;
    for (int ty = y0; ty <= y1 && ty < PK_MAP_H; ty++) {
        for (int tx = x0; tx <= x1 && tx < PK_MAP_W; tx++) {
            const int sx = tx * TILE_PX - cam_x;
            const int sy = ty * TILE_PX - cam_y;
            const pk_tile_t t = (pk_tile_t)g->map.tiles[ty * PK_MAP_W + tx];
            DrawRectangle(sx, sy, TILE_PX, TILE_PX, tile_color(t));
            switch (t) {
                case TILE_TREE:
                    DrawRectangle(sx + 18, sy + 26, 12, 16, (Color){ 92, 62, 40, 255 });
                    DrawCircle(sx + 24, sy + 20, 15, (Color){ 56, 122, 66, 255 });
                    break;
                case TILE_TALL_GRASS:
                    for (int i = 0; i < 4; i++) {
                        DrawLine(sx + 8 + i * 10, sy + 38, sx + 12 + i * 10, sy + 22,
                                 (Color){ 60, 124, 62, 255 });
                    }
                    break;
                case TILE_WATER:
                    DrawLine(sx + 8, sy + 16, sx + 24, sy + 16, (Color){ 150, 196, 236, 255 });
                    DrawLine(sx + 20, sy + 32, sx + 38, sy + 32, (Color){ 150, 196, 236, 255 });
                    break;
                case TILE_ROCK:
                    DrawCircle(sx + 24, sy + 26, 14, (Color){ 112, 106, 100, 255 });
                    break;
                case TILE_FLOWER:
                    DrawCircle(sx + 16, sy + 18, 5, (Color){ 244, 226, 130, 255 });
                    DrawCircle(sx + 32, sy + 30, 5, (Color){ 244, 226, 130, 255 });
                    break;
                case TILE_PATH:
                    DrawCircle(sx + 12, sy + 34, 2, (Color){ 190, 164, 122, 255 });
                    DrawCircle(sx + 34, sy + 14, 2, (Color){ 190, 164, 122, 255 });
                    break;
                case TILE_SHOP:
                case TILE_HEAL:
                case TILE_HOUSE:
                    DrawRectangle(sx + 6, sy + 8, TILE_PX - 12, 14, (Color){ 120, 70, 50, 255 });
                    DrawRectangle(sx + 18, sy + 24, 12, 18, (Color){ 240, 226, 190, 255 });
                    if (t == TILE_SHOP) {
                        DrawText("S", sx + 20, sy + 9, 12, C_TEXT_INV);
                    } else if (t == TILE_HEAL) {
                        DrawText("+", sx + 19, sy + 7, 16, C_TEXT_INV);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    if (g->dest_x != 0xFFFF) {
        const int sx = (int)g->dest_x * TILE_PX - cam_x;
        const int sy = (int)g->dest_y * TILE_PX - cam_y;
        DrawCircleLines(sx + TILE_PX / 2, sy + TILE_PX / 2, 14, C_DANGER);
    }

    {
        const int px = (int)g->world.px * TILE_PX - cam_x + TILE_PX / 2;
        const int py = (int)g->world.py * TILE_PX - cam_y + TILE_PX / 2;
        const int bob = ((g->state.tick / WALK_TICKS) % 2 && g->path_len) ? 2 : 0;
        DrawCircle(px, py + 8 - bob, 12, C_ACCENT);
        DrawCircle(px, py - 6 - bob, 9, (Color){ 244, 214, 180, 255 });
        DrawRectangle(px - 9, py - 18 - bob, 18, 6, C_DANGER);
        const int eye_dx = (g->facing == 2) ? -3 : (g->facing == 3) ? 3 : 0;
        DrawCircle(px - 3 + eye_dx, py - 6 - bob, 1.6f, C_DARK);
        DrawCircle(px + 3 + eye_dx, py - 6 - bob, 1.6f, C_DARK);
    }

    draw_map_hud(g);
    draw_nav(g);

    if (g->msg_ticks > 0 && g->msg[0]) {
        const int w = pk_text_width(g->msg) + 32;
        DrawRectangle((GAME_WIDTH - w) / 2, 56, w, 40, C_BAR_BG);
        pk_draw_text((GAME_WIDTH - pk_text_width(g->msg)) / 2, 68, g->msg,
                     C_TEXT_INV);
    }
}

static void on_enter_tile(game_handle_t g, int x, int y)
{
    pk_world_t *w = &g->world;
    w->steps++;
    const pk_tile_t t = (pk_tile_t)g->map.tiles[y * PK_MAP_W + x];

    if (t == TILE_SHOP) {
        g->page = PAGE_SHOP;
        g->shop_sel = 0;
        wq_sfx_play(WQ_TONE_ENTER);
        return;
    }
    if (t == TILE_HEAL) {
        heal_party_all(w);
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        show_msg(g, "全队回复了！");
        return;
    }

    mon_t *leader = pk_world_leader(w);
    if (leader && leader->hp > 0) {
        encounter_t enc;
        if (pk_world_roll_encounter(w, &g->map, x, y, g->ts, &enc)) {
            dex_mark_seen(&w->dex, enc.species_id, enc.is_shiny);
            battle_begin(g, &enc);
        }
    }
}

static void tap_map(game_handle_t g, int x, int y)
{
    if (nav_tap(g, x, y)) return;
    if (g->msg_ticks > 0) { g->msg_ticks = 0; return; }

    int cam_x, cam_y;
    map_camera(g, &cam_x, &cam_y);
    const int tx = (x + cam_x) / TILE_PX;
    const int ty = (y + cam_y) / TILE_PX;
    if (tx < 0 || ty < 0 || tx >= PK_MAP_W || ty >= PK_MAP_H) return;
    if (tx == (int)g->world.px && ty == (int)g->world.py) return;

    int16_t steps[2 * PK_MAP_W * PK_MAP_H];
    const int n = pk_map_find_path(&g->map, g->world.px, g->world.py, tx, ty,
                                   steps, PK_MAP_W * PK_MAP_H);
    if (n <= 0) {
        show_msg(g, "走不过去");
        return;
    }
    memcpy(g->path, steps, (size_t)n * 2 * sizeof(int16_t));
    g->path_len = n;
    g->path_idx = 0;
    g->walk_phase = 0;
    g->dest_x = (uint16_t)tx;
    g->dest_y = (uint16_t)ty;
}

static void update_walk(game_handle_t g)
{
    if (!g->path_len) return;
    if (++g->walk_phase < WALK_TICKS) return;
    g->walk_phase = 0;

    const int nx = g->path[g->path_idx * 2];
    const int ny = g->path[g->path_idx * 2 + 1];
    const int dx = nx - (int)g->world.px, dy = ny - (int)g->world.py;
    if (dx > 0) g->facing = 3;
    else if (dx < 0) g->facing = 2;
    else if (dy > 0) g->facing = 1;
    else if (dy < 0) g->facing = 0;
    g->world.px = (uint16_t)nx;
    g->world.py = (uint16_t)ny;
    g->path_idx++;
    if (g->path_idx >= g->path_len) {
        g->path_len = 0;
        g->dest_x = 0xFFFF;
    }
    on_enter_tile(g, nx, ny);
}

/* ---------------------------------------------------------------------------
 * BATTLE 渲染
 * --------------------------------------------------------------------------- */

static void battle_draw_box(int x, int y, int w, int h)
{
    DrawRectangle(x, y, w, h, C_PANEL);
    DrawRectangleLines(x, y, w, h, C_DARK);
}

static void render_battle(game_handle_t g)
{
    ClearBackground(C_SKY);

    /* 敌方（右上）：平台 + 前视图。 */
    DrawEllipse(330, 176, 110, 26, C_PANEL_DK);
    draw_mon(g, g->wild_species, g->wild_shiny, false, 330, 110, 2, true);
    {
        char name[24];
        species_name(g->wild_species, name, sizeof(name));
        char buf[48];
        snprintf(buf, sizeof(buf), "%s Lv%u", name, g->wild_level);
        battle_draw_box(12, 12, 210, 54);
        pk_draw_text(24, 20, buf, C_TEXT);
        pk_draw_hp_bar(24, 44, 186, 12, g->bs.wild_hp, g->bs.wild_hp_max);
    }
    if (g->wild_rarity >= 4) {
        pk_draw_text(232, 20, rarity_name(g->wild_rarity), C_DANGER);
    }

    /* 我方（左下）：平台 + 背视图。 */
    DrawEllipse(140, 318, 100, 22, C_PANEL_DK);
    mon_t *leader = pk_world_leader(&g->world);
    if (leader) {
        draw_mon(g, leader->species_id, (leader->flags & 1) != 0, true,
                 140, 258, 2, false);
        char name[24];
        species_name(leader->species_id, name, sizeof(name));
        char buf[48];
        snprintf(buf, sizeof(buf), "%s Lv%u", name, leader->level);
        battle_draw_box(258, 236, 210, 66);
        pk_draw_text(270, 244, buf, C_TEXT);
        pk_draw_hp_bar(270, 268, 186, 12, leader->hp,
                       combat_max_hp(leader->species_id, leader->level));
        snprintf(buf, sizeof(buf), "%u/%u", leader->hp,
                 combat_max_hp(leader->species_id, leader->level));
        DrawText(buf, 270, 284, 14, C_TEXT);
    }

    /* 底部消息/菜单条。 */
    const int box_y = VIEW_H - BP_MSG_H;
    DrawRectangle(0, box_y, GAME_WIDTH, BP_MSG_H, C_DARK);
    if (g->bp == BP_MENU) {
        pk_rect_t btns[4] = {
            { 16, box_y + 8, 108, 48 }, { 132, box_y + 8, 108, 48 },
            { 16, box_y + 62, 108, 44 }, { 132, box_y + 62, 108, 44 },
        };
        const char *labels[4] = { "攻击", "背包", "队伍", "逃跑" };
        for (int i = 0; i < 4; i++) {
            pk_draw_button(&btns[i], labels[i], C_ACCENT, C_TEXT_INV, false);
        }
        /* 投球（顶栏右侧）。 */
        pk_rect_t ball = { GAME_WIDTH - 130, 8, 118, 40 };
        pk_draw_button(&ball, "投球", (Color){ 226, 178, 70, 255 }, C_TEXT, false);
        /* 消息条右侧还显示当前提示。 */
        pk_draw_text(260, box_y + 74, "选择行动", C_TEXT_INV);
    } else if (g->bp == BP_MOVES) {
        const int oy = VIEW_H - BP_OVERLAY_H;
        DrawRectangle(0, oy, GAME_WIDTH, BP_OVERLAY_H, C_PANEL);
        DrawLine(0, oy, GAME_WIDTH, oy, C_DARK);
        for (int i = 0; i < g->battle_move_count; i++) {
            move_t mv;
            if (!combat_move(g->battle_moves[i], &mv)) continue;
            pk_rect_t cell = { 16 + (i % 2) * 224, oy + 12 + (i / 2) * 44, 216, 40 };
            DrawRectangle(cell.x, cell.y, cell.w, cell.h, C_PANEL_DK);
            DrawRectangleLines(cell.x, cell.y, cell.w, cell.h, C_TEXT);
            char label[40];
            snprintf(label, sizeof(label), "%.*s", mv.name_zh_len, mv.name_zh);
            pk_draw_text(cell.x + 10, cell.y + 12, label, C_TEXT);
            char meta[24];
            snprintf(meta, sizeof(meta), "%s P%u", type_name(mv.type), mv.power);
            DrawText(meta, cell.x + 118, cell.y + 14, 14, C_TEXT);
        }
        pk_rect_t back = { GAME_WIDTH - 76, oy + 8, 64, 40 };
        pk_draw_button(&back, "返回", C_DARK, C_TEXT_INV, false);
    } else if (g->bp == BP_BAG || g->bp == BP_SWITCH) {
        const int oy = VIEW_H - BP_OVERLAY_H;
        DrawRectangle(0, oy, GAME_WIDTH, BP_OVERLAY_H, C_PANEL);
        DrawLine(0, oy, GAME_WIDTH, oy, C_DARK);
        if (g->bp == BP_BAG) {
            const uint8_t heals[3] = { ITEM_POTION, ITEM_SUPER_POTION, ITEM_MAX_POTION };
            for (int i = 0; i < 3; i++) {
                const item_info_t *info = items_info(heals[i]);
                pk_rect_t row = { 16, oy + 12 + i * 48, 380, 42 };
                DrawRectangle(row.x, row.y, row.w, row.h, C_PANEL_DK);
                DrawRectangleLines(row.x, row.y, row.w, row.h, C_TEXT);
                char label[48];
                snprintf(label, sizeof(label), "%s x%u",
                         info ? info->name : "?",
                         g->world.bag.quantity[heals[i]]);
                pk_draw_text(row.x + 10, row.y + 13, label, C_TEXT);
            }
        } else {
            for (int i = 1; i < g->world.party.party_count && i <= 5; i++) {
                const mon_t *m = &g->world.party.party[i];
                pk_rect_t row = { 16, oy + 12 + (i - 1) * 36, 380, 32 };
                DrawRectangle(row.x, row.y, row.w, row.h, C_PANEL_DK);
                DrawRectangleLines(row.x, row.y, row.w, row.h, C_TEXT);
                char name[24];
                species_name(m->species_id, name, sizeof(name));
                char label[64];
                snprintf(label, sizeof(label), "%s Lv%u HP%u", name, m->level, m->hp);
                pk_draw_text(row.x + 10, row.y + 8, label, C_TEXT);
            }
        }
        pk_rect_t back = { GAME_WIDTH - 76, oy + 8, 64, 40 };
        pk_draw_button(&back, "返回", C_DARK, C_TEXT_INV, false);
    } else {
        /* INTRO / MSG / END：纯消息。 */
        pk_draw_text(20, box_y + 22, g->battle_msg, C_TEXT_INV);
        pk_draw_text(20, box_y + 58, "点按继续", C_PANEL_DK);
    }
}

/* 战斗子菜单热区（与 render_battle 同几何）。 */
static void tap_battle_ui(game_handle_t g, int x, int y)
{
    if (g->bp != BP_MENU && g->bp != BP_MOVES && g->bp != BP_BAG &&
        g->bp != BP_SWITCH) {
        return;
    }
    const int box_y = VIEW_H - BP_MSG_H;
    const int oy = VIEW_H - BP_OVERLAY_H;

    if (g->bp == BP_MENU) {
        pk_rect_t ball = { GAME_WIDTH - 130, 8, 118, 40 };
        if (pk_hit(&ball, x, y)) {
            if (g->world.bag.quantity[ITEM_POKE] <= 0 &&
                g->world.bag.quantity[ITEM_GREAT] <= 0 &&
                g->world.bag.quantity[ITEM_ULTRA] <= 0) {
                show_msg(g, "没有球了");
                return;
            }
            g->page = PAGE_CAPTURE;
            g->cp = CP_AIM;
            g->cap_start_tick = (int)g->state.tick;
            g->cap_ball = (g->world.bag.quantity[ITEM_ULTRA] > 0) ? ITEM_ULTRA
                        : (g->world.bag.quantity[ITEM_GREAT] > 0) ? ITEM_GREAT
                        : ITEM_POKE;
            wq_sfx_play(WQ_TONE_ENTER);
            return;
        }
        pk_rect_t btns[4] = {
            { 16, box_y + 8, 108, 48 }, { 132, box_y + 8, 108, 48 },
            { 16, box_y + 62, 108, 44 }, { 132, box_y + 62, 108, 44 },
        };
        for (int i = 0; i < 4; i++) {
            if (!pk_hit(&btns[i], x, y)) continue;
            wq_sfx_play(WQ_TONE_MOVE);
            if (i == 0) {
                g->bp = BP_MOVES;
            } else if (i == 1) {
                g->bp = BP_BAG;
            } else if (i == 2) {
                g->bp = BP_SWITCH;
            } else {
                battle_escape_result_t esc;
                battle_escape_try(&g->bs, &esc);
                if (esc.escaped) {
                    show_msg(g, "成功逃走了！");
                    g->page = PAGE_MAP;
                    wq_sfx_play(WQ_TONE_BACK);
                } else {
                    snprintf(g->battle_msg, sizeof(g->battle_msg), "没能逃走！");
                    g->battle_pending_n = 0;
                    (void)battle_step_push(g);
                    if (g->bs.finished) {
                        battle_finish(g);
                        g->battle_pending_n = 0;
                        snprintf(g->battle_msg, sizeof(g->battle_msg), "战败... 被送回了回复之家");
                        g->bp = BP_END;
                    } else {
                        g->bp = BP_MSG;
                    }
                }
            }
            return;
        }
        return;
    }

    pk_rect_t back = { GAME_WIDTH - 76, oy + 8, 64, 40 };
    if (pk_hit(&back, x, y)) {
        g->bp = BP_MENU;
        wq_sfx_play(WQ_TONE_BACK);
        return;
    }

    if (g->bp == BP_MOVES) {
        for (int i = 0; i < g->battle_move_count; i++) {
            pk_rect_t cell = { 16 + (i % 2) * 224, oy + 12 + (i / 2) * 44, 216, 40 };
            if (!pk_hit(&cell, x, y)) continue;
            g->bs.player_move = g->battle_moves[i];
            g->bs.player_move_set = true;
            g->bs.auto_battle = true;
            wq_sfx_play(WQ_TONE_PLACE);
            battle_run_round(g);
            return;
        }
        return;
    }
    if (g->bp == BP_BAG) {
        const uint8_t heals[3] = { ITEM_POTION, ITEM_SUPER_POTION, ITEM_MAX_POTION };
        for (int i = 0; i < 3; i++) {
            pk_rect_t row = { 16, oy + 12 + i * 48, 380, 42 };
            if (!pk_hit(&row, x, y)) continue;
            if (g->world.bag.quantity[heals[i]] <= 0) {
                show_msg(g, "没有这个道具");
                return;
            }
            mon_t *ld = pk_world_leader(&g->world);
            if (!ld) return;
            item_use_result_t res;
            const item_use_status_t st = items_apply(heals[i], ld->species_id,
                                                     ld->level,
                                                     &g->world.nurture, &res);
            if (st != ITEM_USE_OK) {
                show_msg(g, "现在不能用");
                return;
            }
            const uint16_t max = g->bs.pet_hp_max;
            if (ld->hp >= max) {
                show_msg(g, "体力已满");
                return;
            }
            g->world.bag.quantity[heals[i]]--;
            uint16_t heal = heals[i] == ITEM_POTION ? 20
                          : heals[i] == ITEM_SUPER_POTION ? 50 : max;
            if (ld->hp + heal > max) heal = (uint16_t)(max - ld->hp);
            ld->hp = (uint16_t)(ld->hp + heal);
            g->bs.pet_hp = ld->hp;
            save_game(g);
            snprintf(g->battle_msg, sizeof(g->battle_msg), "回复了体力！");
            g->battle_pending_n = 0;
            g->bp = BP_MSG;
            wq_sfx_play(WQ_TONE_CORRECT);
            return;
        }
        return;
    }
    /* BP_SWITCH */
    for (int i = 1; i < g->world.party.party_count && i <= 5; i++) {
        pk_rect_t row = { 16, oy + 12 + (i - 1) * 36, 380, 32 };
        if (!pk_hit(&row, x, y)) continue;
        mon_t *m = &g->world.party.party[i];
        if (m->hp <= 0) {
            show_msg(g, "这只没有体力");
            return;
        }
        const mon_t tmp = g->world.party.party[0];
        g->world.party.party[0] = *m;
        g->world.party.party[i] = tmp;
        const uint16_t wild_hp = g->bs.wild_hp;
        mon_t *ld = pk_world_leader(&g->world);
        memset(&g->bs, 0, sizeof(g->bs));
        battle_session_init(&g->bs, ld->species_id, ld->level, g->wild_species,
                            g->wild_level, leader_ability_q10(&g->world),
                            g->world.rng);
        g->world.rng = g->world.rng * 1664525u + 1013904223u;
        g->bs.wild_hp = wild_hp;
        g->bs.started = true;
        g->bs.auto_battle = true;
        battle_reload_moves(g);
        char name[24];
        species_name(ld->species_id, name, sizeof(name));
        snprintf(g->battle_msg, sizeof(g->battle_msg), "换%s上场！", name);
        g->battle_pending_n = 0;
        g->bp = BP_MSG;
        save_game(g);
        wq_sfx_play(WQ_TONE_ENTER);
        return;
    }
}

static void tap_battle(game_handle_t g, int x, int y)
{
    switch (g->bp) {
        case BP_INTRO:
            g->bp = BP_MENU;
            battle_reload_moves(g);
            wq_sfx_play(WQ_TONE_ENTER);
            break;
        case BP_MSG:
            battle_advance_msg(g);
            break;
        case BP_END:
            g->page = PAGE_MAP;
            wq_sfx_play(WQ_TONE_BACK);
            pump_growth(g);
            break;
        default:
            tap_battle_ui(g, x, y);
            break;
    }
}

/* ---------------------------------------------------------------------------
 * CAPTURE
 * --------------------------------------------------------------------------- */

static void render_capture(game_handle_t g)
{
    ClearBackground((Color){ 232, 222, 196, 255 });
    draw_mon(g, g->wild_species, g->wild_shiny, false, GAME_WIDTH / 2, 150, 3, true);
    char name[24];
    species_name(g->wild_species, name, sizeof(name));
    char buf[48];
    snprintf(buf, sizeof(buf), "%s Lv%u%s", name, g->wild_level,
             g->wild_shiny ? " 闪光" : "");
    pk_draw_text((GAME_WIDTH - pk_text_width(buf)) / 2, 252, buf, C_TEXT);

    const int bar_y = 330;
    DrawRectangle(CAP_BAR_X, bar_y, CAP_BAR_W, 36, C_PANEL_DK);
    DrawRectangleLines(CAP_BAR_X, bar_y, CAP_BAR_W, 36, C_DARK);

    species_t sp;
    const bool has_sp = assets_species(g->wild_species, &sp);
    if (g->cp == CP_AIM && has_sp) {
        mon_t *leader = pk_world_leader(&g->world);
        cap_context_t ctx = { leader ? leader->level : 5, g->wild_level,
                              combat_max_hp(g->wild_species, g->wild_level) / 8,
                              sp.weight_hg };
        const uint8_t hp_ratio = battle_session_hp_ratio(&g->bs);
        const uint16_t ww = cap_window_width_context(
            sp.catch_rate, nurture_ability_factor(&g->world.nurture),
            (cap_ball_t)g->cap_ball, hp_ratio, &ctx);
        const uint16_t start200 = (uint16_t)((CAP_BAR_WIDTH - ww) / 2);
        DrawRectangle(CAP_BAR_X + start200 * 2, bar_y, ww * 2, 36,
                      (Color){ 120, 200, 120, 255 });
        const uint32_t elapsed = (uint32_t)(g->state.tick - g->cap_start_tick) * 33u;
        const uint16_t p = cap_pointer_position(elapsed);
        DrawRectangle(CAP_BAR_X + p * 2 - 3, bar_y - 8, 6, 52, C_DANGER);
        pk_draw_text(CAP_BAR_X, bar_y - 34, "在绿框里点按投球！", C_TEXT);
    } else if (g->cp == CP_RESULT) {
        pk_draw_text(CAP_BAR_X, bar_y - 34,
                     g->cap_res.caught ? "抓住了！" : g->cap_res.fled ? "它逃走了..." : "没有抓住...",
                     g->cap_res.caught ? C_ACCENT : C_DANGER);
    }

    pk_rect_t hint = { 90, 396, 300, 44 };
    pk_draw_button(&hint, g->cp == CP_AIM ? "点按投球" : "继续", C_ACCENT,
                   C_TEXT_INV, false);
}

static void tap_capture(game_handle_t g, int x, int y)
{
    (void)x;
    (void)y;
    if (g->cp == CP_RESULT) {
        g->msg_ticks = 0;
        if (g->cap_res.caught) {
            g->page = PAGE_MAP;
        } else if (g->cap_res.fled || g->bs.finished) {
            /* 逃走或已被打倒：直接回地图（战败结算在此不重复）。 */
            if (g->bs.finished && !g->bs.won && !g->bs.pet_hp) {
                battle_finish(g);
            }
            g->page = PAGE_MAP;
        } else {
            /* 失败未逃：敌方反击一次，回战斗页。 */
            g->page = PAGE_BATTLE;
            g->battle_pending_n = 0;
            snprintf(g->battle_msg, sizeof(g->battle_msg), "球被弹开了！");
            (void)battle_step_push(g);
            if (g->bs.finished) {
                battle_finish(g);
                g->battle_pending_n = 0;
                snprintf(g->battle_msg, sizeof(g->battle_msg), "战败... 被送回了回复之家");
                g->bp = BP_END;
            } else {
                g->bp = BP_MSG;
            }
        }
        return;
    }
    /* CP_AIM：点哪儿都是投球。 */
    if (g->msg_ticks > 0) { g->msg_ticks = 0; return; }

    pk_world_t *w = &g->world;
    species_t sp;
    if (!assets_species(g->wild_species, &sp)) return;
    mon_t *leader = pk_world_leader(w);
    cap_context_t ctx = { leader ? leader->level : 5, g->wild_level,
                          combat_max_hp(g->wild_species, g->wild_level) / 8,
                          sp.weight_hg };
    const uint32_t elapsed = (uint32_t)(g->state.tick - g->cap_start_tick) * 33u;
    const uint16_t mood_q10 = nurture_ability_factor(&w->nurture);

    cap_attempt_context(sp.catch_rate, mood_q10, (cap_ball_t)g->cap_ball,
                        battle_session_hp_ratio(&g->bs), g->wild_rarity,
                        elapsed, w->rng, &ctx, &g->cap_res);
    w->rng = w->rng * 1664525u + 1013904223u;
    if (w->bag.quantity[g->cap_ball] > 0) w->bag.quantity[g->cap_ball]--;
    g->cp = CP_RESULT;

    if (g->cap_res.caught) {
        w->catches++;
        dex_mark_caught(&w->dex, g->wild_species, g->wild_shiny);
        mon_t caught = {0};
        caught.species_id = (uint8_t)g->wild_species;
        caught.level = g->wild_level;
        caught.hp = combat_max_hp(caught.species_id, caught.level);
        caught.intimacy = 40;
        const bool to_party = party_receive(&w->party, &caught);
        if (!to_party) {
            w->party.box[caught.species_id - 1] = caught;   /* 满了进仓库 */
        }
        w->money += 50;
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        show_msg(g, to_party ? "加入了队伍！" : "队伍满了，送进了仓库");
    } else {
        wq_sfx_play(g->cap_res.fled ? WQ_TONE_WRONG : WQ_TONE_BACK);
    }
}

/* ---------------------------------------------------------------------------
 * DEX
 * --------------------------------------------------------------------------- */

static void render_dex(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title("图鉴");
    char buf[40];
    snprintf(buf, sizeof(buf), "目击 %u 捕获 %u", dex_count_seen(&g->world.dex),
             dex_count_caught(&g->world.dex));
    pk_draw_text(280, 14, buf, C_TEXT);

    const int grid_top = 52;
    int max_row = (151 + DEX_COLS - 1) / DEX_COLS - DEX_ROWS_VISIBLE;
    if (max_row < 0) max_row = 0;
    if (g->dex_top > max_row) g->dex_top = max_row;
    if (g->dex_top < 0) g->dex_top = 0;
    for (int row = 0; row < DEX_ROWS_VISIBLE; row++) {
        for (int col = 0; col < DEX_COLS; col++) {
            const int idx = (g->dex_top + row) * DEX_COLS + col;
            if (idx < 0 || idx >= 151) continue;
            const uint16_t sid = (uint16_t)(idx + 1);
            const int cx = 10 + col * (DEX_CELL + 4);
            const int cy = grid_top + row * (DEX_CELL + 4);
            const bool seen = dex_is_seen(&g->world.dex, sid);
            const bool caught = dex_is_caught(&g->world.dex, sid);
            DrawRectangle(cx, cy, DEX_CELL, DEX_CELL,
                          caught ? (Color){ 208, 232, 196, 255 } : C_PANEL_DK);
            DrawRectangleLines(cx, cy, DEX_CELL, DEX_CELL, C_TEXT);
            if (seen) {
                species_t sp;
                if (assets_species(sid, &sp)) {
                    uint16_t pal[4];
                    assets_palette_variant(sp.palette, false, pal);
                    uint8_t size;
                    const uint8_t *data = assets_front_sprite(sid, &size);
                    if (data) {
                        pk_draw_sprite_thumbnail(cx + 16, cy + 16, data, size, 56, pal);
                    }
                }
            } else {
                DrawText("?", cx + DEX_CELL / 2 - 8, cy + 26, 28, C_TEXT);
            }
            snprintf(buf, sizeof(buf), "%u", sid);
            DrawText(buf, cx + 6, cy + 4, 14, C_TEXT);
            if (dex_is_shiny_caught(&g->world.dex, sid)) {
                DrawCircle(cx + DEX_CELL - 12, cy + 12, 5, GOLD);
            }
        }
    }
    if (max_row > 0) {
        const int track_h = VIEW_H - grid_top - 8;
        const int thumb_h = track_h * DEX_ROWS_VISIBLE / (max_row + DEX_ROWS_VISIBLE);
        const int thumb_y = grid_top + (track_h - thumb_h) * g->dex_top / max_row;
        DrawRectangle(GAME_WIDTH - 8, grid_top, 5, track_h, C_PANEL_DK);
        DrawRectangle(GAME_WIDTH - 8, thumb_y, 5, thumb_h, C_ACCENT);
    }
    draw_nav(g);
}

static void tap_dex(game_handle_t g, int x, int y)
{
    if (nav_tap(g, x, y)) return;
    if (y < 52) return;
    const int max_row = ((151 + DEX_COLS - 1) / DEX_COLS - DEX_ROWS_VISIBLE) > 0
                            ? (151 + DEX_COLS - 1) / DEX_COLS - DEX_ROWS_VISIBLE : 0;
    for (int row = 0; row < DEX_ROWS_VISIBLE; row++) {
        for (int col = 0; col < DEX_COLS; col++) {
            const int idx = (g->dex_top + row) * DEX_COLS + col;
            if (idx < 0 || idx >= 151) continue;
            pk_rect_t cell = { 10 + col * (DEX_CELL + 4), 52 + row * (DEX_CELL + 4),
                               DEX_CELL, DEX_CELL };
            if (!pk_hit(&cell, x, y)) continue;
            const uint16_t sid = (uint16_t)(idx + 1);
            if (!dex_is_seen(&g->world.dex, sid) || g->dex_moved) return;
            g->dex_sel = idx;
            g->page = PAGE_DEX_DETAIL;
            wq_sfx_play(WQ_TONE_ENTER);
            return;
        }
    }
    (void)max_row;
}

static void render_dex_detail(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title_at("图鉴", 84);
    draw_back_btn();
    const uint16_t sid = (uint16_t)(g->dex_sel + 1);
    species_t sp;
    if (!assets_species(sid, &sp)) return;
    draw_mon(g, sid, dex_is_shiny_caught(&g->world.dex, sid), false,
             GAME_WIDTH / 2, 170, 3, true);
    char buf[64];
    snprintf(buf, sizeof(buf), "No.%u %.*s", sid, sp.name_zh_len, sp.name_zh);
    pk_draw_text_scaled((GAME_WIDTH - pk_text_width_scaled(buf, 2)) / 2, 280, buf,
                        C_TEXT, 2);
    if (dex_is_caught(&g->world.dex, sid)) {
        snprintf(buf, sizeof(buf), "属性 %s%s  种族值和 %u",
                 type_name(sp.type1),
                 sp.type2 != TY_NONE ? type_name(sp.type2) : "",
                 sp.hp + sp.attack + sp.defense + sp.special + sp.speed);
        pk_draw_text(24, 334, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "体力 %u  攻击 %u  防御 %u", sp.hp, sp.attack,
                 sp.defense);
        pk_draw_text(24, 362, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "特攻 %u  速度 %u  捕获率 %u", sp.special,
                 sp.speed, sp.catch_rate);
        pk_draw_text(24, 390, buf, C_TEXT);
    } else {
        pk_draw_text(24, 344, "尚未捕获", C_TEXT);
        pk_draw_text(24, 372, "去草丛里找找吧", C_TEXT);
    }
}

static void tap_dex_detail(game_handle_t g, int x, int y)
{
    if (back_hit(x, y)) {
        g->page = PAGE_DEX;
        wq_sfx_play(WQ_TONE_BACK);
    }
}

/* ---------------------------------------------------------------------------
 * PARTY
 * --------------------------------------------------------------------------- */

static void render_party(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title("队伍");
    char buf[48];
    snprintf(buf, sizeof(buf), "%u/6", g->world.party.party_count);
    pk_draw_text(360, 14, buf, C_TEXT);
    for (int i = 0; i < g->world.party.party_count; i++) {
        const mon_t *m = &g->world.party.party[i];
        const int y = 56 + i * 60;
        DrawRectangle(12, y, 456, 54,
                      i == 0 ? (Color){ 208, 232, 196, 255 } : C_PANEL_DK);
        DrawRectangleLines(12, y, 456, 54, C_TEXT);
        species_t sp;
        if (assets_species(m->species_id, &sp)) {
            uint16_t pal[4];
            assets_palette_variant(sp.palette, (m->flags & 1) != 0, pal);
            uint8_t size;
            const uint8_t *data = assets_front_sprite(m->species_id, &size);
            if (data) pk_draw_sprite_thumbnail(20, y + 7, data, size, 40, pal);
            snprintf(buf, sizeof(buf), "%.*s", sp.name_zh_len, sp.name_zh);
            pk_draw_text(76, y + 6, buf, C_TEXT);
        }
        if (m->flags & 1) DrawCircle(66, y + 12, 5, GOLD);
        snprintf(buf, sizeof(buf), "Lv%u", m->level);
        DrawText(buf, 210, y + 8, 18, C_TEXT);
        pk_draw_hp_bar(280, y + 10, 130, 12, m->hp,
                       combat_max_hp(m->species_id, m->level));
        snprintf(buf, sizeof(buf), "%u/%u", m->hp,
                 combat_max_hp(m->species_id, m->level));
        DrawText(buf, 414, y + 8, 14, C_TEXT);
        if (i == 0) pk_draw_text(76, y + 30, "队长", C_ACCENT);
    }
    draw_nav(g);
}

static void tap_party(game_handle_t g, int x, int y)
{
    if (nav_tap(g, x, y)) return;
    for (int i = 0; i < g->world.party.party_count; i++) {
        pk_rect_t row = { 12, 56 + i * 60, 456, 54 };
        if (!pk_hit(&row, x, y)) continue;
        g->party_sel = i;
        g->page = PAGE_PARTY_DETAIL;
        wq_sfx_play(WQ_TONE_ENTER);
        return;
    }
}

static void render_party_detail(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title_at("详情", 84);
    draw_back_btn();
    if (g->party_sel >= g->world.party.party_count) return;
    const mon_t *m = &g->world.party.party[g->party_sel];
    species_t sp;
    if (!m->species_id || !assets_species(m->species_id, &sp)) return;

    draw_mon(g, m->species_id, (m->flags & 1) != 0, false, 100, 140, 2, true);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*s", sp.name_zh_len, sp.name_zh);
    pk_draw_text_scaled(200, 84, buf, C_TEXT, 2);
    snprintf(buf, sizeof(buf), "Lv%u  %s%s", m->level, type_name(sp.type1),
             sp.type2 != TY_NONE ? type_name(sp.type2) : "");
    pk_draw_text(200, 124, buf, C_TEXT);
    if (m->flags & 1) pk_draw_text(200, 150, "闪光", C_DANGER);

    uint32_t got, need;
    exp_progress(m->exp, m->level, &got, &need);
    pk_draw_text(200, 176, "经验", C_TEXT);
    pk_draw_hp_bar(250, 178, 160, 10, (int)got, (int)need);

    pk_draw_text(200, 202, "体力", C_TEXT);
    pk_draw_hp_bar(250, 204, 160, 10, m->hp,
                   combat_max_hp(m->species_id, m->level));
    snprintf(buf, sizeof(buf), "攻击 %u  防御 %u",
             combat_stat(m->species_id, m->level, COMBAT_ATTACK),
             combat_stat(m->species_id, m->level, COMBAT_DEFENSE));
    pk_draw_text(200, 228, buf, C_TEXT);
    snprintf(buf, sizeof(buf), "特攻 %u  速度 %u",
             combat_stat(m->species_id, m->level, COMBAT_SP_ATTACK),
             combat_stat(m->species_id, m->level, COMBAT_SPEED));
    pk_draw_text(200, 252, buf, C_TEXT);
    snprintf(buf, sizeof(buf), "亲密 %u  旅程 %u", m->intimacy,
             m->explore_value > 999 ? 999 : m->explore_value);
    pk_draw_text(200, 276, buf, C_TEXT);

    pk_rect_t lead = { 60, 330, 160, 52 };
    pk_rect_t heal = { 260, 330, 160, 52 };
    pk_draw_button(&lead, g->party_sel == 0 ? "已是队长" : "设为队长",
                   g->party_sel == 0 ? C_PANEL_DK : C_ACCENT, C_TEXT_INV, false);
    snprintf(buf, sizeof(buf), "回复水 x%u", g->world.bag.quantity[ITEM_POTION]);
    pk_draw_button(&heal, buf, C_ACCENT, C_TEXT_INV, false);
}

static void tap_party_detail(game_handle_t g, int x, int y)
{
    if (back_hit(x, y)) {
        g->page = PAGE_PARTY;
        wq_sfx_play(WQ_TONE_BACK);
        return;
    }
    pk_rect_t lead = { 60, 330, 160, 52 };
    pk_rect_t heal = { 260, 330, 160, 52 };
    if (pk_hit(&lead, x, y) && g->party_sel > 0) {
        const mon_t tmp = g->world.party.party[0];
        g->world.party.party[0] = g->world.party.party[g->party_sel];
        g->world.party.party[g->party_sel] = tmp;
        save_game(g);
        wq_sfx_play(WQ_TONE_ENTER);
        return;
    }
    if (pk_hit(&heal, x, y)) {
        mon_t *m = &g->world.party.party[g->party_sel];
        if (g->world.bag.quantity[ITEM_POTION] <= 0) {
            show_msg(g, "没有回复水");
            return;
        }
        const uint16_t max = combat_max_hp(m->species_id, m->level);
        if (m->hp >= max) {
            show_msg(g, "体力已满");
            return;
        }
        m->hp = (uint16_t)(m->hp + 20 > max ? max : m->hp + 20);
        g->world.bag.quantity[ITEM_POTION]--;
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        show_msg(g, "回复了！");
    }
}

/* ---------------------------------------------------------------------------
 * BAG
 * --------------------------------------------------------------------------- */

static void render_bag(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title("背包");
    int row = 0;
    char buf[64];
    for (uint8_t it = 0; it < ITEM_COUNT && row < 8; it++) {
        if (g->world.bag.quantity[it] == 0) continue;
        const int y = 56 + row * 44;
        const item_info_t *info = items_info(it);
        DrawRectangle(12, y, 456, 40, C_PANEL_DK);
        DrawRectangleLines(12, y, 456, 40, C_TEXT);
        pk_draw_text(24, y + 12, info ? info->name : "?", C_TEXT);
        snprintf(buf, sizeof(buf), "x%u", g->world.bag.quantity[it]);
        DrawText(buf, 350, y + 12, 16, C_TEXT);
        row++;
    }
    if (row == 0) pk_draw_text(180, 200, "背包是空的", C_TEXT);
    pk_draw_text(16, VIEW_H - 26, "点选使用，球要在战斗里投", C_TEXT);
    draw_nav(g);
}

static void tap_bag(game_handle_t g, int x, int y)
{
    if (nav_tap(g, x, y)) return;
    int row = 0;
    for (uint8_t it = 0; it < ITEM_COUNT && row < 8; it++) {
        if (g->world.bag.quantity[it] == 0) continue;
        pk_rect_t r = { 12, 56 + row * 44, 456, 40 };
        if (pk_hit(&r, x, y)) {
            const item_info_t *info = items_info(it);
            if (!info) return;
            if (info->kind == ITEM_KIND_BALL) {
                show_msg(g, "球要在战斗里投");
                wq_sfx_play(WQ_TONE_WRONG);
                return;
            }
            if (info->kind == ITEM_KIND_HEAL) {
                show_msg(g, "回复类道具在队伍页对伙伴使用");
                return;
            }
            if (info->kind == ITEM_KIND_CARE) {
                mon_t *leader = pk_world_leader(&g->world);
                if (!leader) return;
                item_use_result_t res;
                const item_use_status_t st = items_apply(it, leader->species_id,
                                                         leader->level,
                                                         &g->world.nurture, &res);
                if (st == ITEM_USE_OK) {
                    g->world.bag.quantity[it]--;
                    g->world.nurture = res.after;
                    if (res.species_after != res.species_before) {
                        leader->species_id = (uint8_t)res.species_after;
                        leader->hp = combat_max_hp(leader->species_id, leader->level);
                        dex_mark_caught(&g->world.dex, res.species_after, false);
                        show_msg(g, "进化了！");
                    } else {
                        show_msg(g, "使用了%s", info->name);
                    }
                    save_game(g);
                    wq_sfx_play(WQ_TONE_CORRECT);
                } else {
                    wq_sfx_play(WQ_TONE_WRONG);
                    show_msg(g, "现在不能用");
                }
                return;
            }
            /* 石头 / 机器 → 选目标。 */
            g->bag_pending_item = it;
            g->page = PAGE_BAG_TARGET;
            return;
        }
        row++;
    }
}

static void render_bag_target(game_handle_t g)
{
    ClearBackground(C_PANEL);
    const item_info_t *info = items_info(g->bag_pending_item);
    draw_page_title(info ? info->name : "使用");
    draw_back_btn();
    pk_draw_text(84, 14, "选一只伙伴使用", C_TEXT);
    for (int i = 0; i < g->world.party.party_count; i++) {
        const mon_t *m = &g->world.party.party[i];
        const int y = 84 + i * 58;
        DrawRectangle(12, y, 456, 52, C_PANEL_DK);
        DrawRectangleLines(12, y, 456, 52, C_TEXT);
        species_t sp;
        if (assets_species(m->species_id, &sp)) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%.*s Lv%u", sp.name_zh_len, sp.name_zh,
                     m->level);
            pk_draw_text(24, y + 16, buf, C_TEXT);
        }
    }
}

static void tap_bag_target(game_handle_t g, int x, int y)
{
    if (back_hit(x, y)) {
        g->page = PAGE_BAG;
        wq_sfx_play(WQ_TONE_BACK);
        return;
    }
    for (int i = 0; i < g->world.party.party_count; i++) {
        pk_rect_t row = { 12, 84 + i * 58, 456, 52 };
        if (!pk_hit(&row, x, y)) continue;
        mon_t *m = &g->world.party.party[i];
        const item_info_t *info = items_info(g->bag_pending_item);
        item_use_result_t res;
        const item_use_status_t st = items_apply(g->bag_pending_item,
                                                 m->species_id, m->level,
                                                 &g->world.nurture, &res);
        if (st == ITEM_USE_OK) {
            g->world.bag.quantity[g->bag_pending_item]--;
            g->world.nurture = res.after;
            if (res.species_after != res.species_before) {
                dex_mark_seen(&g->world.dex, m->species_id, false);
                m->species_id = (uint8_t)res.species_after;
                m->hp = combat_max_hp(m->species_id, m->level);
                dex_mark_caught(&g->world.dex, res.species_after, false);
                show_msg(g, "进化了！");
                wq_sfx_play(WQ_TONE_COMPLETE);
            } else {
                show_msg(g, "使用了%s", info ? info->name : "道具");
                wq_sfx_play(WQ_TONE_CORRECT);
            }
            save_game(g);
        } else {
            wq_sfx_play(WQ_TONE_WRONG);
            show_msg(g, st == ITEM_USE_LEVEL_TOO_LOW ? "等级还不够"
                      : st == ITEM_USE_NOT_APPLICABLE ? "对这只没用"
                      : "现在不能用");
        }
        g->page = PAGE_BAG;
        return;
    }
}

/* ---------------------------------------------------------------------------
 * SHOP（交易所）
 * --------------------------------------------------------------------------- */

static void render_shop(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title_at("市集", 84);
    draw_back_btn();
    char buf[32];
    snprintf(buf, sizeof(buf), "金钱 %lu", (unsigned long)g->world.money);
    pk_draw_text(300, 14, buf, C_TEXT);
    for (int i = 0; i < SHOP_STOCK_COUNT; i++) {
        const int y = 56 + i * 40;
        if (y > VIEW_H - 48) break;
        const item_info_t *info = items_info(SHOP_STOCK[i].item);
        DrawRectangle(12, y, 380, 36, C_PANEL_DK);
        DrawRectangleLines(12, y, 380, 36, C_TEXT);
        pk_draw_text(24, y + 10, info ? info->name : "?", C_TEXT);
        snprintf(buf, sizeof(buf), "$%lu", (unsigned long)SHOP_STOCK[i].price);
        DrawText(buf, 300, y + 10, 16, C_TEXT);
        pk_rect_t buy = { 400, y, 68, 36 };
        pk_draw_button(&buy, "+1", C_ACCENT, C_TEXT_INV, false);
    }
}

static void tap_shop(game_handle_t g, int x, int y)
{
    if (back_hit(x, y)) {
        g->page = PAGE_MAP;
        wq_sfx_play(WQ_TONE_BACK);
        return;
    }
    for (int i = 0; i < SHOP_STOCK_COUNT; i++) {
        pk_rect_t buy = { 400, 56 + i * 40, 68, 36 };
        if (!pk_hit(&buy, x, y)) continue;
        const shop_entry_t *e = &SHOP_STOCK[i];
        if (g->world.money < e->price) {
            show_msg(g, "金钱不够");
            wq_sfx_play(WQ_TONE_WRONG);
            return;
        }
        if (g->world.bag.quantity[e->item] >= items_capacity(e->item)) {
            show_msg(g, "背包放不下了");
            return;
        }
        g->world.money -= e->price;
        g->world.bag.quantity[e->item]++;
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * CARE（照料）
 * --------------------------------------------------------------------------- */

static void render_care(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title_at("照料", 84);
    draw_back_btn();
    const pk_world_t *w = &g->world;
    const mon_t *leader = pk_world_leader(&g->world);
    if (!leader) return;
    char name[24];
    species_name(leader->species_id, name, sizeof(name));
    draw_mon(g, leader->species_id, (leader->flags & 1) != 0, false, 90, 140, 2, true);
    pk_draw_text_scaled(180, 84, name, C_TEXT, 2);

    struct { const char *name; int32_t v; } axes[4] = {
        { "饱食", w->nurture.satiety },
        { "心情", w->nurture.mood },
        { "体能", w->nurture.stamina },
        { "亲密", w->nurture.intimacy },
    };
    for (int i = 0; i < 4; i++) {
        const int y = 210 + i * 34;
        pk_draw_text(40, y, axes[i].name, C_TEXT);
        pk_draw_hp_bar(110, y + 2, 240, 16, nurture_pct(axes[i].v), 100);
        char buf[12];
        snprintf(buf, sizeof(buf), "%u%%", nurture_pct(axes[i].v));
        DrawText(buf, 364, y + 2, 16, C_TEXT);
    }
    static const char *MOODS[] = { "愉快", "平静", "低落", "消沉" };
    char buf[32];
    snprintf(buf, sizeof(buf), "心情 %s", MOODS[nurture_mood(&w->nurture)]);
    pk_draw_text(110, 352, buf, C_TEXT);

    pk_rect_t feed = { 40, 390, 120, 48 };
    pk_rect_t play = { 180, 390, 120, 48 };
    pk_rect_t rest = { 320, 390, 120, 48 };
    pk_draw_button(&feed, "喂食", C_ACCENT, C_TEXT_INV, false);
    pk_draw_button(&play, "玩耍", C_ACCENT, C_TEXT_INV, false);
    pk_draw_button(&rest, "休息", C_ACCENT, C_TEXT_INV, false);
}

static void tap_care(game_handle_t g, int x, int y)
{
    if (back_hit(x, y)) {
        g->page = PAGE_MORE;
        wq_sfx_play(WQ_TONE_BACK);
        return;
    }
    pk_world_t *w = &g->world;
    pk_rect_t feed = { 40, 390, 120, 48 };
    pk_rect_t play = { 180, 390, 120, 48 };
    pk_rect_t rest = { 320, 390, 120, 48 };
    if (pk_hit(&feed, x, y)) {
        if (w->bag.quantity[ITEM_BERRY] <= 0) {
            show_msg(g, "没有树果了，去市集看看");
            wq_sfx_play(WQ_TONE_WRONG);
            return;
        }
        w->bag.quantity[ITEM_BERRY]--;
        nurture_feed(&w->nurture);
        mon_t *leader = pk_world_leader(w);
        if (leader && leader->intimacy < 255) leader->intimacy += 2;
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        show_msg(g, "吃饱了！");
        return;
    }
    if (pk_hit(&play, x, y)) {
        if (!nurture_play(&w->nurture)) {
            show_msg(g, "体能不够了，先休息");
            wq_sfx_play(WQ_TONE_WRONG);
            return;
        }
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        show_msg(g, "玩得很开心！");
        return;
    }
    if (pk_hit(&rest, x, y)) {
        nurture_rest(&w->nurture);
        save_game(g);
        wq_sfx_play(WQ_TONE_ENTER);
        show_msg(g, "休息一下...");
    }
}

/* ---------------------------------------------------------------------------
 * MORE（我的）
 * --------------------------------------------------------------------------- */

static void render_more(game_handle_t g)
{
    ClearBackground(C_PANEL);
    draw_page_title("我的");
    char buf[80];
    pk_rect_t care = { 40, 60, 190, 52 };
    pk_rect_t shop = { 250, 60, 190, 52 };
    pk_rect_t save = { 40, 124, 190, 52 };
    pk_rect_t vol  = { 250, 124, 190, 52 };
    pk_draw_button(&care, "照料", C_ACCENT, C_TEXT_INV, false);
    pk_draw_button(&shop, "市集", C_ACCENT, C_TEXT_INV, false);
    pk_draw_button(&save, "保存进度", C_ACCENT, C_TEXT_INV, false);
    snprintf(buf, sizeof(buf), "音量档 %u", game_volume_level());
    pk_draw_button(&vol, buf, C_ACCENT, C_TEXT_INV, false);

    pk_draw_text(40, 210, "旅程记录", C_TEXT);
    snprintf(buf, sizeof(buf), "步数 %lu   战胜 %u   捕获 %u",
             (unsigned long)g->world.steps, g->world.wins, g->world.catches);
    pk_draw_text(40, 244, buf, C_TEXT);
    snprintf(buf, sizeof(buf), "图鉴 目击 %u / 捕获 %u / 151",
             dex_count_seen(&g->world.dex), dex_count_caught(&g->world.dex));
    pk_draw_text(40, 272, buf, C_TEXT);
    snprintf(buf, sizeof(buf), "金钱 %lu", (unsigned long)g->world.money);
    pk_draw_text(40, 300, buf, C_TEXT);
    pk_draw_text(40, 356, "草丛与深草里会遇到野生伙伴", C_TEXT);
    pk_draw_text(40, 384, "北部森林与高山地带出稀有种类", C_TEXT);
    draw_nav(g);
}

static void tap_more(game_handle_t g, int x, int y)
{
    if (nav_tap(g, x, y)) return;
    pk_rect_t care = { 40, 60, 190, 52 };
    pk_rect_t shop = { 250, 60, 190, 52 };
    pk_rect_t save = { 40, 124, 190, 52 };
    pk_rect_t vol  = { 250, 124, 190, 52 };
    if (pk_hit(&care, x, y)) {
        g->page = PAGE_CARE;
        wq_sfx_play(WQ_TONE_ENTER);
    } else if (pk_hit(&shop, x, y)) {
        g->page = PAGE_SHOP;
        wq_sfx_play(WQ_TONE_ENTER);
    } else if (pk_hit(&save, x, y)) {
        save_game(g);
        wq_sfx_play(WQ_TONE_CORRECT);
        show_msg(g, "已保存");
    } else if (pk_hit(&vol, x, y)) {
        game_volume_cycle();
    }
}

/* 音量档位（wq_volume 档位表管理，游戏层只做循环与下发）。 */
static uint8_t s_volume = WQ_VOLUME_DEFAULT;
uint8_t game_volume_level(void) { return s_volume; }
void game_volume_cycle(void)
{
    s_volume = wq_volume_next(s_volume);
    wq_sfx_set_volume(s_volume);
    if (s_volume != WQ_VOLUME_OFF) wq_sfx_play(WQ_TONE_MOVE);
}

/* ---------------------------------------------------------------------------
 * EVOLVE 演出
 * --------------------------------------------------------------------------- */

static void render_evolve_overlay(game_handle_t g)
{
    DrawRectangle(0, 0, GAME_WIDTH, GAME_HEIGHT, (Color){ 20, 24, 30, 220 });
    const int phase = (g->evo.tick / 10) % 2;
    const bool flash = (g->evo.tick % 20) < 6;
    const uint8_t show = (uint8_t)((flash || phase) ? g->evo.to : g->evo.from);
    species_t sp;
    if (!assets_species(show, &sp)) return;
    uint16_t pal[4];
    assets_palette_variant(sp.palette, false, pal);
    uint8_t size;
    const uint8_t *data = assets_front_sprite(show, &size);
    if (data) {
        const int scale = 3 + (g->evo.tick % 30) / 15;
        pk_draw_sprite_2bpp(GAME_WIDTH / 2 - size * scale / 2,
                            VIEW_H / 2 - size * scale / 2, data, size, size,
                            scale, pal);
    }
    if (g->evo.done) {
        pk_draw_text_scaled((GAME_WIDTH - pk_text_width_scaled("进化了！", 2)) / 2,
                            400, "进化了！", C_TEXT_INV, 2);
        pk_draw_text((GAME_WIDTH - pk_text_width("点按继续")) / 2, 440,
                     "点按继续", C_PANEL_DK);
    } else {
        pk_draw_text((GAME_WIDTH - pk_text_width("...")) / 2, 400, "...", C_TEXT_INV);
    }
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * --------------------------------------------------------------------------- */

bool game_create(const game_config_t *config, game_handle_t *ret_handle)
{
    if (!ret_handle) return false;
    *ret_handle = NULL;
    if (!config || !config->width || !config->height) return false;
    game_handle_t g = calloc(1, sizeof(*g));
    if (!g) return false;
    g->config = *config;
    g->dest_x = 0xFFFF;

    if (!assets_init()) {
        PK_LOGE("game", "assets_init failed - game logic unusable");
    }
    (void)pk_font_init();
    pk_map_generate(&g->map);
    pk_world_init(&g->world);

    (void)pk_store_init();
    uint8_t blob[PK_SAVE_BLOB_SIZE];
    uint32_t len = 0;
    if (pk_store_load(blob, sizeof(blob), &len) == ESP_OK && len > 0 &&
        pk_world_deserialize(&g->world, blob, len)) {
        g->page = PAGE_MAP;
        g->tab = TAB_MAP;
        PK_LOGI("game", "save loaded: px=%u py=%u party=%u",
                g->world.px, g->world.py, g->world.party.party_count);
    } else {
        pk_world_init(&g->world);
        g->page = PAGE_STARTER;
    }

    (void)wq_sfx_start();
    wq_sfx_set_volume(s_volume);
    *ret_handle = g;
    return true;
}

void game_delete(game_handle_t g)
{
    if (!g) return;
    save_game(g);
    wq_sfx_stop();
    free(g);
}

void game_reset(game_handle_t g)
{
    if (!g) return;
    pk_map_generate(&g->map);
    pk_world_init(&g->world);
    g->page = PAGE_STARTER;
    g->tab = TAB_MAP;
    g->path_len = 0;
    g->msg_ticks = 0;
    g->dest_x = 0xFFFF;
    g->evo.active = false;
    (void)pk_store_erase();
}

void game_set_paused(game_handle_t g, bool paused)
{
    if (g) g->state.paused = paused;
}

void game_set_pointer(game_handle_t game, int32_t x, int32_t y, bool pressed)
{
    if (!game) return;
    game->state.pointer_x = x < 0 ? 0 : (x >= game->config.width ? game->config.width - 1 : x);
    game->state.pointer_y = y < 0 ? 0 : (y >= game->config.height ? game->config.height - 1 : y);
    const bool was_down = game->state.pointer_down;
    game->state.pointer_down = pressed;
    const int px = (int)game->state.pointer_x;
    const int py = (int)game->state.pointer_y;

    /* 图鉴拖动（按下/移动/抬起都在这条路径上）。 */
    if (game->page == PAGE_DEX && y < VIEW_H && py >= 52) {
        if (pressed && !was_down) {
            game->dex_dragging = true;
            game->dex_moved = false;
            game->dex_drag_y = py;
            game->dex_drag_anchor = game->dex_top;
            return;
        }
        if (pressed && game->dex_dragging) {
            const int row_h = DEX_CELL + 4;
            int top = game->dex_drag_anchor - (py - game->dex_drag_y) / row_h;
            const int max_row = (151 + DEX_COLS - 1) / DEX_COLS - DEX_ROWS_VISIBLE;
            if (top > max_row) top = max_row;
            if (top < 0) top = 0;
            if (top != game->dex_top) game->dex_moved = true;
            game->dex_top = top;
            return;
        }
        if (!pressed && game->dex_dragging) {
            game->dex_dragging = false;
            return;
        }
    }
    if (!pressed || was_down) return;

    /* 进化演出覆盖一切输入。 */
    if (game->evo.active) {
        if (game->evo.done) {
            apply_evolution(game);
            wq_sfx_play(WQ_TONE_CORRECT);
        }
        return;
    }

    switch (game->page) {
        case PAGE_STARTER:      tap_starter(game, px, py); break;
        case PAGE_MAP:          tap_map(game, px, py); break;
        case PAGE_BATTLE:       tap_battle(game, px, py); break;
        case PAGE_CAPTURE:      tap_capture(game, px, py); break;
        case PAGE_DEX:          tap_dex(game, px, py); game->dex_moved = false; break;
        case PAGE_DEX_DETAIL:   tap_dex_detail(game, px, py); break;
        case PAGE_PARTY:        tap_party(game, px, py); break;
        case PAGE_PARTY_DETAIL: tap_party_detail(game, px, py); break;
        case PAGE_BAG:          tap_bag(game, px, py); break;
        case PAGE_BAG_TARGET:   tap_bag_target(game, px, py); break;
        case PAGE_SHOP:         tap_shop(game, px, py); break;
        case PAGE_CARE:         tap_care(game, px, py); break;
        case PAGE_MORE:         tap_more(game, px, py); break;
        default:                break;
    }
}

void game_update(game_handle_t g)
{
    if (!g || g->state.paused) return;
    ++g->state.tick;
    if (g->state.tick % 30 == 0) g->ts++;
    if (g->msg_ticks > 0) g->msg_ticks--;

    pk_world_nurture_tick(&g->world, (int64_t)g->state.tick * 33333LL);

    if (g->page == PAGE_MAP) update_walk(g);

    if (g->evo.active) {
        g->evo.tick++;
        if (!g->evo.done && g->evo.tick > 100) g->evo.done = true;
    }
}

bool game_render(game_handle_t g)
{
    if (!g) return false;
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return false;
    }

    switch (g->page) {
        case PAGE_STARTER:      render_starter(g); break;
        case PAGE_MAP:          render_map(g); break;
        case PAGE_BATTLE:       render_battle(g); break;
        case PAGE_CAPTURE:      render_capture(g); break;
        case PAGE_DEX:          render_dex(g); break;
        case PAGE_DEX_DETAIL:   render_dex_detail(g); break;
        case PAGE_PARTY:        render_party(g); break;
        case PAGE_PARTY_DETAIL: render_party_detail(g); break;
        case PAGE_BAG:          render_bag(g); break;
        case PAGE_BAG_TARGET:   render_bag_target(g); break;
        case PAGE_SHOP:         render_shop(g); break;
        case PAGE_CARE:         render_care(g); break;
        case PAGE_MORE:         render_more(g); break;
        default:                ClearBackground(C_PANEL); break;
    }

    if (g->evo.active) render_evolve_overlay(g);

    EndDrawing();
    return true;
}

game_snapshot_t game_read(game_handle_t g)
{
    return g ? g->state : (game_snapshot_t){0};
}

// main/assets.c —— 读嵌进固件的资产。
//
// 六个 .bin 用 CMake 的 EMBED_FILES 直接嵌进 app 分区（158.8 KB，
// 占 3MB 分区的 5%），不另开数据分区 —— 那样要多一套分区表与
// 烧写步骤，而 158 KB 完全放得下。
//
// ## 这是「PC 参考实现 → 固件」的第一次真移植
//
// 格式定义在 tools/pipeline/convert_*.py，PC 侧的读取实现在
// sim/systems.py 与 tools/inspector/build.py。三处必须一致 ——
// 而「一致」不能靠人记，所以：
//
//   · 每个 parse 函数都校验 magic 与定长自洽，不对就整表置空
//   · assets_selftest() 把关键数字打到串口，与 PC 侧
//     `python3 tools/pipeline/inventory_assets.py` 的输出对账
//
// 上一轮踩过的坑：convert_gen1.py 的注释把记录长度写成 28（实际 32），
// 照那个数写固件会让第 2 条记录起整体错位 4 字节 —— 而错位后读出的
// 仍是合法数值（种族值、属性 id 都在 0~255 内），不崩，只是全错。
// 所以这里的每个 REC_SIZE 都从**文件头读**，不写死。

#include <string.h>

#include "pk_port.h"
#include "pk_assets.h"

#include "assets.h"
#include "combat.h"
#include "pokemon_names.h"

static const char *TAG = "assets";

// 资产字节来源见 pk_assets.h：设备端是链接期嵌入的 flash 符号，
// 宿主模拟器是运行时从磁盘读进来的缓冲。解析代码两边共用。
#define gen1_bin_start  (pk_asset("gen1")->data)
#define gen1_bin_end    (pk_asset("gen1")->data + pk_asset("gen1")->size)
#define front_bin_start (pk_asset("gen1_front")->data)
#define front_bin_end   (pk_asset("gen1_front")->data + pk_asset("gen1_front")->size)
#define back_bin_start  (pk_asset("gen1_back")->data)
#define back_bin_end    (pk_asset("gen1_back")->data + pk_asset("gen1_back")->size)
#define pal_bin_start   (pk_asset("palettes")->data)
#define pal_bin_end     (pk_asset("palettes")->data + pk_asset("palettes")->size)
#define ui_bin_start    (pk_asset("ui")->data)
#define ui_bin_end      (pk_asset("ui")->data + pk_asset("ui")->size)
#define moves_bin_start (pk_asset("moves")->data)
#define moves_bin_end   (pk_asset("moves")->data + pk_asset("moves")->size)
#define font_bin_start  (pk_asset("font16")->data)
#define font_bin_end    (pk_asset("font16")->data + pk_asset("font16")->size)

// 小端读取。资产全是小端（convert_*.py 用 struct "<"），
// 而 C3 也是小端，理论上可以直接强转指针 —— **但不要那么做**：
// 记录不保证 2/4 字节对齐（gen1 记录 32 字节但字段错位排列），
// 未对齐访问在 RISC-V 上是异常而非静默慢速。
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// gen1.bin —— 151 只物种数据
// ---------------------------------------------------------------------------

static struct {
    const uint8_t *recs;
    const uint8_t *pool;
    uint16_t rec_size;
    uint32_t count;
    bool ok;
} s_gen1;

static bool parse_gen1(void)
{
    const uint8_t *d = gen1_bin_start;
    size_t len = (size_t)(gen1_bin_end - gen1_bin_start);
    if (len < 16 || memcmp(d, "GEN1", 4) != 0) return false;

    uint16_t rsz = rd16(d + 6);
    uint32_t cnt = rd32(d + 8);
    uint32_t pool_sz = rd32(d + 12);
    // 自洽校验：头部声明的三段长度加起来必须正好等于文件长度。
    // 不校验的后果是越界读 —— 而 flash 上越界读不会崩，
    // 只会读到隔壁资产的字节当成种族值。
    if (16u + cnt * rsz + pool_sz != len) {
        PK_LOGE(TAG, "gen1.bin 长度不自洽: 16+%u*%u+%u != %u",
                 (unsigned)cnt, rsz, (unsigned)pool_sz, (unsigned)len);
        return false;
    }
    s_gen1.recs = d + 16;
    s_gen1.pool = d + 16 + cnt * rsz;
    s_gen1.rec_size = rsz;
    s_gen1.count = cnt;
    s_gen1.ok = true;
    return true;
}

bool assets_species(uint16_t id, species_t *out)
{
    if (!s_gen1.ok || id < 1 || id > s_gen1.count || !out) return false;
    const uint8_t *r = s_gen1.recs + (uint32_t)(id - 1) * s_gen1.rec_size;

    // 字段偏移见 tools/pipeline/convert_gen1.py 顶部的 off/size 表。
    // **记录长度 32 字节**（那份注释一度写成 28，照抄会整体错位）。
    out->id = id;
    out->type1 = r[3];
    out->type2 = r[4];
    out->biome_mask = r[5];
    out->catch_rate = r[6];
    out->evolve_trigger = r[7];
    out->evolve_to = r[8];
    out->evolve_level = r[9];
    out->hp = r[10];
    out->attack = r[11];
    out->defense = r[12];
    out->special = r[13];
    out->speed = r[14];
    out->flags = r[15];
    out->weight_hg = rd16(r + 18);
    // 调色板索引 8 位（上限 256 套）。原来只取低 4 位（& 0x0F），
    // 上限 16 套；GSC 有 135 套，4 位装不下。高 4 位经全仓 grep 确认
    // 无其他字段占用（convert_gen1.py 的 off/size 表标注「高 4 位预留」）。
    out->palette = r[23];

    uint16_t zo = rd16(r + 20);
    uint8_t zl = r[22];
    out->name_zh = (const char *)(s_gen1.pool + zo);
    out->name_zh_len = zl;
    uint8_t selected_length = 0;
    const char *selected_name = pokemon_names_override(id, &selected_length);
    if (selected_name) {
        out->name_zh = selected_name;
        out->name_zh_len = selected_length;
    }
    return true;
}

uint32_t assets_species_count(void) { return s_gen1.ok ? s_gen1.count : 0; }

// ---------------------------------------------------------------------------
// moves.bin —— 伤害招 + 151 只的升级学习表
// ---------------------------------------------------------------------------

static struct {
    const uint8_t *moves;
    const uint8_t *species;
    const uint8_t *learn;
    const uint8_t *pool;
    uint16_t rec_size;
    uint16_t move_count;
    uint16_t species_count;
    uint16_t learn_count;
    bool ok;
} s_mv;

static bool parse_moves(void)
{
    const uint8_t *d = moves_bin_start;
    size_t len = (size_t)(moves_bin_end - moves_bin_start);
    if (len < 16 || memcmp(d, "MOVE", 4) != 0) return false;

    uint16_t rsz = rd16(d + 6);
    uint16_t mcnt = rd16(d + 8);
    uint16_t scnt = rd16(d + 10);
    uint16_t lcnt = rd16(d + 12);
    uint16_t psz = rd16(d + 14);
    size_t need = 16u + (size_t)mcnt * rsz + (size_t)scnt * 4 +
                  (size_t)lcnt * 2 + psz;
    if (need != len) {
        PK_LOGE(TAG, "moves.bin 长度不自洽: %u != %u",
                 (unsigned)need, (unsigned)len);
        return false;
    }
    s_mv.moves = d + 16;
    s_mv.species = s_mv.moves + (size_t)mcnt * rsz;
    s_mv.learn = s_mv.species + (size_t)scnt * 4;
    s_mv.pool = s_mv.learn + (size_t)lcnt * 2;
    s_mv.rec_size = rsz;
    s_mv.move_count = mcnt;
    s_mv.species_count = scnt;
    s_mv.learn_count = lcnt;
    s_mv.ok = true;
    return true;
}

bool assets_move(uint16_t id, move_t *out) {return combat_move(id,out);}

int assets_known_moves(uint16_t species_id,uint8_t level,move_t *out,int max_out)
{
 if(!out||max_out<=0)return 0;
 uint16_t ids[COMBAT_MOVE_CAP];int n=combat_known_moves(species_id,level,ids,COMBAT_MOVE_CAP);
 if(n>max_out)n=max_out;
 for(int i=0;i<n;i++)combat_move(ids[i],&out[i]);
 return n;
}

uint16_t assets_move_count(void) { return COMBAT_MOVE_CAP; }

// ---------------------------------------------------------------------------
// sprite / 调色板 / 字库 —— 先只做长度校验与取指针，
// 渲染留到 UI 那步（现在没有页面消费它们，提前写就是没验证过的代码）
// ---------------------------------------------------------------------------

static struct {
    const uint8_t *d;
    uint16_t count, per;
    uint8_t w, h;
    bool ok;
} s_back;

static bool parse_back(void)
{
    const uint8_t *d = back_bin_start;
    size_t len = (size_t)(back_bin_end - back_bin_start);
    s_back.ok = false;
    if (len < 16 || memcmp(d, "BACK", 4) != 0) return false;
    uint16_t w = rd16(d + 6), h = rd16(d + 8), per = rd16(d + 10);
    uint32_t cnt = rd32(d + 12);
    if (h != w || (w != 32 && w != 48) ||
        per != (w + 3u) / 4u * h || cnt > UINT16_MAX ||
        16u + (size_t)cnt * per != len) {
        PK_LOGE(TAG, "back.bin 不自洽: %ux%u per=%u cnt=%u len=%u",
                 w, h, per, (unsigned)cnt, (unsigned)len);
        return false;
    }
    s_back.d = d + 16;
    s_back.count = (uint16_t)cnt;
    s_back.per = per;
    s_back.w = (uint8_t)w;
    s_back.h = (uint8_t)h;
    s_back.ok = true;
    return true;
}

const uint8_t *assets_back_sprite(uint16_t id)
{
    // Existing callers still assume 32x32; never hand them a 48px record.
    if (!s_back.ok || s_back.w != 32 || id < 1 || id > s_back.count) return NULL;
    return s_back.d + (size_t)(id - 1) * s_back.per;
}

bool assets_back_sprite_info(uint16_t id, sprite_asset_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s_back.ok || id < 1 || id > s_back.count) return false;
    out->data = s_back.d + (size_t)(id - 1) * s_back.per;
    out->w = s_back.w;
    out->h = s_back.h;
    return true;
}

#define FRONT_MAX_SEGMENTS 3
#define FRONT_SEG_BYTES 12

typedef struct {
    const uint8_t *records;
    uint16_t size;
    uint16_t per;
    uint32_t count;
} front_segment_t;

static struct {
    front_segment_t segments[FRONT_MAX_SEGMENTS];
    uint16_t count;
    bool ok;
} s_front;

static bool parse_front(void)
{
    s_front.ok = false;
    const uint8_t *d = front_bin_start;
    size_t len = (size_t)(front_bin_end - front_bin_start);
    if (len < 8 || memcmp(d, "FRNT", 4) != 0 || rd16(d + 4) != 1) {
        return false;
    }

    uint16_t segments = rd16(d + 6);
    size_t table_bytes = (size_t)segments * FRONT_SEG_BYTES;
    if (segments == 0 || segments > FRONT_MAX_SEGMENTS ||
        8u + table_bytes > len) {
        return false;
    }

    const uint8_t *blob = d + 8 + table_bytes;
    size_t blob_len = len - (8 + table_bytes);
    for (uint16_t i = 0; i < segments; i++) {
        const uint8_t *e = d + 8 + (size_t)i * FRONT_SEG_BYTES;
        uint16_t size = rd16(e);
        uint16_t per = rd16(e + 2);
        uint32_t count = rd32(e + 4);
        uint32_t off = rd32(e + 8);
        size_t records_len = (size_t)count * (2u + per);

        if ((size != 40 && size != 48 && size != 56) ||
            per != ((uint32_t)size * size + 3u) / 4u ||
            off > blob_len || records_len > blob_len - off) {
            PK_LOGE(TAG, "front.bin segment %u invalid", i);
            return false;
        }
        s_front.segments[i] = (front_segment_t){
            .records = blob + off,
            .size = size,
            .per = per,
            .count = count,
        };
    }

    s_front.count = segments;
    s_front.ok = true;
    return true;
}

const uint8_t *assets_front_sprite(uint16_t id, uint8_t *size)
{
    if (size) *size = 0;
    if (!size || !s_front.ok || id < 1 || id > assets_species_count()) return NULL;

    // Presentation art can change generations while gameplay records remain
    // byte-for-byte stable. The FRNT record ID, not the old flags hint, is authoritative.
    for (uint16_t tier = 0; tier < s_front.count; tier++) {
        const front_segment_t *seg = &s_front.segments[tier];
        for (uint32_t i = 0; i < seg->count; i++) {
            const uint8_t *record = seg->records + (size_t)i * (2u + seg->per);
            if (rd16(record) == id) {
                *size = (uint8_t)seg->size;
                return record + 2;
            }
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// UI 点阵素材 —— 格式见 tools/pipeline/convert_ui.py
// ---------------------------------------------------------------------------

#define UI_NAME_LEN 16
#define UI_ENTRY_SIZE 26          // 16 名字 + w + h + off(4) + len(2) + pad(2)

static struct { const uint8_t *tab, *blob; uint16_t count; bool ok; } s_ui;

static bool parse_ui(void)
{
    const uint8_t *d = ui_bin_start;
    size_t len = (size_t)(ui_bin_end - ui_bin_start);
    if (len < 8 || memcmp(d, "UIA1", 4) != 0) return false;
    uint16_t cnt = rd16(d + 6);
    size_t need = 8 + (size_t)cnt * UI_ENTRY_SIZE;
    if (len < need) {
        PK_LOGE(TAG, "ui.bin 截断: %u 条目要 %u B，只有 %u",
                 cnt, (unsigned)need, (unsigned)len);
        return false;
    }
    s_ui.tab = d + 8;
    s_ui.blob = d + need;
    s_ui.count = cnt;
    s_ui.ok = true;
    return true;
}

bool assets_ui(const char *name, ui_art_t *out)
{
    if (!s_ui.ok || !name || !out) return false;
    for (uint16_t i = 0; i < s_ui.count; i++) {
        const uint8_t *e = s_ui.tab + (size_t)i * UI_ENTRY_SIZE;
        // 名字定长 16 且尾部补零 —— strncmp 到 16 字节就够，
        // 不用 strcmp（表里的名字不保证以 \0 结尾时 strcmp 会越界读）
        if (strncmp((const char *)e, name, UI_NAME_LEN) != 0) continue;
        out->w = e[UI_NAME_LEN];
        out->h = e[UI_NAME_LEN + 1];
        uint32_t off = rd32(e + UI_NAME_LEN + 2);
        out->data = s_ui.blob + off;
        return true;
    }
    return false;
}

static struct {
    const uint8_t *colors;
    uint16_t pairs;
    bool ok;
} s_pal;

static bool parse_palettes(void)
{
    s_pal.ok = false;
    const uint8_t *d = pal_bin_start;
    size_t len = (size_t)(pal_bin_end - pal_bin_start);
    if (len < 12 || memcmp(d, "PALS", 4) != 0 || rd16(d + 4) != 1 || rd16(d + 8) != 4)
        return false;
    uint16_t pairs = rd16(d + 6), count = rd16(d + 10);
    size_t index_offset = 12u + (size_t)pairs * 16u;
    if (!pairs || pairs > 256 || count != assets_species_count() || index_offset + count != len)
        return false;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t index = d[index_offset + i];
        if (index >= pairs || index != s_gen1.recs[(size_t)i * s_gen1.rec_size + 23]) return false;
    }
    s_pal.colors = d + 12;
    s_pal.pairs = pairs;
    s_pal.ok = true;
    return true;
}

void assets_palette_variant(uint8_t set_idx, bool shiny, uint16_t out[4])
{
    if (!out) return;
    out[0] = out[1] = out[2] = 0;
    out[3] = 0xffff;
    if (!s_pal.ok || set_idx >= s_pal.pairs) return;
    size_t index = (size_t)set_idx + (shiny ? s_pal.pairs : 0u);
    const uint8_t *p = s_pal.colors + index * 8u;
    for (int i = 0; i < 4; i++) out[i] = rd16(p + i * 2);
}

void assets_palette(uint8_t set_idx, uint16_t out[4])
{
    assets_palette_variant(set_idx, false, out);
}

// ---------------------------------------------------------------------------

bool assets_init(void)
{
    bool ok = true;
    ok &= parse_gen1();
    ok &= parse_moves();
    ok &= parse_back();
    ok &= parse_front();
    ok &= parse_palettes();
    ok &= parse_ui();
    if (!ok) PK_LOGE(TAG, "资产解析失败 —— 游戏逻辑不可用");
    return ok;
}

void assets_selftest(void)
{
    // 这些数字要与 PC 侧对得上：
    //   python3 tools/pipeline/inventory_assets.py
    // 对不上就是格式漂移了 —— 那是最难查的一类 bug，
    // 因为长度、magic 都对，只有内容错位。
    PK_LOGI(TAG, "gen1 %u 只 · moves %u 招 · back %u 张",
             (unsigned)assets_species_count(),
             (unsigned)assets_move_count(),
             (unsigned)(s_back.ok ? s_back.count : 0));

    // UI 素材：**逐个查名字**，而不是只报个总数。
    // 名字是字符串键，拼错在编译期查不出来 —— 只有查一遍才知道。
    // 尺寸也一起验：506 字节的东西错一个字节就全歪了。
    static const struct { const char *n; uint8_t w, h; } UI_EXPECT[] = {
        // Crystal source canvas is 32x32; the closed ball's visible ink is 24x24.
        {"ball_24", 32, 32}, {"ball_open", 32, 32}, {"cursor", 5, 9},
        {"heart", 7, 6}, {"star_5", 5, 5}, {"star_7", 7, 7},
    };
    int ui_ok = 0;
    for (size_t i = 0; i < sizeof(UI_EXPECT) / sizeof(UI_EXPECT[0]); i++) {
        ui_art_t a;
        if (!assets_ui(UI_EXPECT[i].n, &a)) {
            PK_LOGE(TAG, "UI 素材缺失: %s", UI_EXPECT[i].n);
        } else if (a.w != UI_EXPECT[i].w || a.h != UI_EXPECT[i].h) {
            PK_LOGE(TAG, "UI %s 尺寸 %ux%u ≠ 期望 %ux%u", UI_EXPECT[i].n,
                     a.w, a.h, UI_EXPECT[i].w, UI_EXPECT[i].h);
        } else {
            ui_ok++;
        }
    }
    PK_LOGI(TAG, "UI 素材 %d/6（精灵球·光标·心形·星星）", ui_ok);

    species_t sp;
    if (assets_species(25, &sp)) {
        PK_LOGI(TAG, "#025 种族值 %u/%u/%u/%u/%u  catch=%u  → 期望 35/55/40/50/90 catch=190",
                 sp.hp, sp.attack, sp.defense, sp.special, sp.speed,
                 sp.catch_rate);
    } else {
        PK_LOGE(TAG, "#025 读取失败");
    }

    move_t mv[8];
    int n = assets_known_moves(25, 43, mv, 8);
    PK_LOGI(TAG, "皮卡丘 Lv43 等级技能（前8项）%d", n);
    for (int i = 0; i < n; i++) {
        PK_LOGI(TAG, "  %.*s  威力%u 命中%u 数据PP%u %s",
                 mv[i].name_zh_len, mv[i].name_zh,
                 mv[i].power, mv[i].accuracy, mv[i].pp,
                 mv[i].special ? "特殊" : "物理");
    }

    // 无招物种的兜底 —— 凯西只会变化招，应当退回「挣扎」
    n = assets_known_moves(63, 20, mv, 8);
    PK_LOGI(TAG, "凯西 Lv20 → %d 招 %.*s", n,
             n ? mv[0].name_zh_len : 0, n ? mv[0].name_zh : "");
}

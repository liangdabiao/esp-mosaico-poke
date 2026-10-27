// main/assets.h —— 嵌进固件的资产读取。
//
// 格式定义在 tools/pipeline/convert_*.py，PC 侧读取实现在
// sim/systems.py。三处必须一致 —— assets_selftest() 把关键数字
// 打到串口，与 `python3 tools/pipeline/inventory_assets.py` 对账。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Legacy 32x32 view only. Current Crystal backs are 48x48; use the sized API.
#define BACK_SPRITE_BYTES 256

// 「挣扎」的 PokeAPI 招式 id。无招可用时的兜底，与
// sim/systems.py 的 STRUGGLE_MOVE_ID 必须一致。
#define MOVE_ID_STRUGGLE 165

typedef struct {
    uint16_t id;
    uint8_t type1, type2;        // TYPES 下标；type2 == 0xFF 表示无
    uint8_t biome_mask;
    uint8_t catch_rate;
    uint8_t evolve_trigger;      // 0=升级 1=道具 2=交换 0xFF=不进化
    uint8_t evolve_to;
    uint8_t evolve_level;
    uint8_t hp, attack, defense, special, speed;   // 初代单一 Special
    uint8_t flags;               // bit0=传说 bit1=幻兽；历史尺寸提示保留，实际尺寸读FRNT
    uint16_t weight_hg;          // Existing GEN1 record offset 18, hectograms.
    uint8_t palette;
    const char *name_zh;         // 当前译名的不可变存储，按长度读取，不依赖 NUL
    uint8_t name_zh_len;         // 用 "%.*s" 打印
} species_t;

typedef struct {
    uint16_t id;
    uint8_t type;                // TYPES 下标
    uint8_t power;
    uint8_t accuracy;            // 255 = 必中
    uint8_t pp;
    bool special;                // 特殊招用 special 双向，物理招用 atk/def
    const char *name_zh;         // 招式资产字符串池，非 NUL 结尾
    uint8_t name_zh_len;
} move_t;

// 解析全部资产。失败时后续查询返回空 —— 调用方要判。
bool assets_init(void);

// 把关键数字打到串口，与 PC 侧 inventory_assets.py 对账
void assets_selftest(void);

bool assets_species(uint16_t id, species_t *out);
uint32_t assets_species_count(void);

// All level-eligible moves, deduplicated and including evolutionary ancestors.
// Calculated from the full learnset; no four-move limit or PP consumption.
// Returns at most max_out entries, ordered by move ID.
int assets_known_moves(uint16_t species_id, uint8_t level,
                       move_t *out, int max_out);
uint16_t assets_move_count(void);
bool assets_move(uint16_t id, move_t *out);

typedef struct {
    const uint8_t *data;      // 2bpp rows, ceil(w/4) bytes per row
    uint8_t w, h;
} sprite_asset_t;

// Sized back view. Dimensions and record stride come from the BACK header.
// Invalid assets/IDs return false and clear out. Supports 32x32 and 48x48.
bool assets_back_sprite_info(uint16_t id, sprite_asset_t *out);

// Legacy 32x32 view (256 bytes). Other sizes/invalid IDs return NULL.
const uint8_t *assets_back_sprite(uint16_t id);

// front sprite 按图集显式物种 ID 返回 2bpp 数据，并把实际边长写入 size。
// 图集损坏、物种缺失或 id 越界时返回 NULL，size 写 0。
const uint8_t *assets_front_sprite(uint16_t id, uint8_t *size);

// ---------------------------------------------------------------------------
// UI 点阵素材（ui.bin）—— 精灵球、光标、心形、星星
//
// 这些是 sim/pixelart.py 算法生成的，不是外部素材：
//   · 光标 —— 字库里没有 ▸（PingFang 不含该字形），必须用点阵
//   · 心形 —— 同上，♥ 也不在
//   · 精灵球 —— P4 捕获页的主角，之前只有文字「精灵球 ×12」
// 全套 506 字节。
//
// 按**名字**查而不是下标：素材个位数，线性查找的常数远小于
// 「下标改了忘同步」的代价。名字见 tools/pipeline/convert_ui.py 的 collect()。
typedef struct {
    const uint8_t *data;      // 2bpp，与 sprite 同格式
    uint8_t w, h;
} ui_art_t;

// 找一个素材。找不到返回 false（调用方要判 —— 名字拼错是编译期查不出的）。
bool assets_ui(const char *name, ui_art_t *out);

// 取一套 sprite 调色板（RGB565，4 色）。set_idx 来自 species_t.palette。
//
// **提到这里而不是各页自己读** —— P1/P3/P4 都要画 sprite，
// 三份拷贝里只要有一份把偏移算错（头 12 字节 + 每组 8 字节），
// 那一页的颜色就是错的，而另外两页正常 —— 极难注意到。
void assets_palette(uint8_t set_idx, uint16_t out[4]);

// Original paired palette variant. The shiny offset is computed in size_t,
// so >128 palette pairs never wrap at an 8-bit set index. Invalid IDs are black/white.
void assets_palette_variant(uint8_t set_idx, bool shiny, uint16_t out[4]);

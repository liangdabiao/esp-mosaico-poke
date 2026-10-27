// pk/pk_assets_device.c —— 设备端资产来源：链接期嵌入的 flash 符号。
//
// CMakeLists 用 target_add_binary_data 嵌入 main/assets/*.bin，符号名由
// 文件名推导（gen1.bin -> _binary_gen1_bin_start/end），与原版 EMBED_FILES
// 的命名规则一致。
#include <string.h>

#include "pk_assets.h"

extern const uint8_t _binary_gen1_bin_start[];
extern const uint8_t _binary_gen1_bin_end[];
extern const uint8_t _binary_gen1_front_bin_start[];
extern const uint8_t _binary_gen1_front_bin_end[];
extern const uint8_t _binary_gen1_back_bin_start[];
extern const uint8_t _binary_gen1_back_bin_end[];
extern const uint8_t _binary_palettes_bin_start[];
extern const uint8_t _binary_palettes_bin_end[];
extern const uint8_t _binary_ui_bin_start[];
extern const uint8_t _binary_ui_bin_end[];
extern const uint8_t _binary_moves_bin_start[];
extern const uint8_t _binary_moves_bin_end[];
extern const uint8_t _binary_font16_bin_start[];
extern const uint8_t _binary_font16_bin_end[];

const pk_blob_t *pk_asset(const char *name)
{
    static const struct {
        const char *name;
        const uint8_t *start;
        const uint8_t *end;
    } TABLE[] = {
        { "gen1",       _binary_gen1_bin_start,       _binary_gen1_bin_end },
        { "gen1_front", _binary_gen1_front_bin_start, _binary_gen1_front_bin_end },
        { "gen1_back",  _binary_gen1_back_bin_start,  _binary_gen1_back_bin_end },
        { "palettes",   _binary_palettes_bin_start,   _binary_palettes_bin_end },
        { "ui",         _binary_ui_bin_start,         _binary_ui_bin_end },
        { "moves",      _binary_moves_bin_start,      _binary_moves_bin_end },
        { "font16",     _binary_font16_bin_start,     _binary_font16_bin_end },
    };
    static pk_blob_t blobs[sizeof(TABLE) / sizeof(TABLE[0])];
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
        if (strcmp(name, TABLE[i].name) != 0) continue;
        if (!blobs[i].data) {
            blobs[i].data = TABLE[i].start;
            blobs[i].size = (size_t)(TABLE[i].end - TABLE[i].start);
        }
        return &blobs[i];
    }
    return NULL;
}

/* 设备端资产在 flash 里，无需运行时登记根目录。 */
void pk_assets_set_root(const char *root)
{
    (void)root;
}

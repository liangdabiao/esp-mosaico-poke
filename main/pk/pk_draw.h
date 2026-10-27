// pk/pk_draw.h —— 渲染小件：FNT1 中文字库 + 2bpp 精灵 + 通用控件。
//
// 自原版 render.c 移植并适配 Raylib Lite：
//   · 字库：font16.bin（1bpp 16×16，ASCII 半宽 8px 步进），逐行按
//     「墨迹跨度」用 DrawRectangle 画 —— 逐像素 DrawPixel 太慢，
//     跨度数每字形只有几个，480 屏上整行文本也只是一二十个矩形。
//   · 精灵：2bpp（每字节 4 像素，高位在左，3=透明）+ RGB565 调色板，
//     同样按跨度画。缩放用最近邻（GB 风格，双线性会糊掉硬边缘）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "raylib.h"
/* DrawRectangle 等必须走 MosaicoFast 映射（设备端是 RGB565 surface 后端）。 */
#include "mosaico_raylib_fast.h"

// 初始化字库（assets_init 之后调用）。失败时文本绘制静默为空。
bool pk_font_init(void);
uint16_t pk_font_size(void);

// 一个 UTF-8 码点的前进量：ASCII 8px，CJK 16px。
uint8_t pk_char_advance(uint16_t cp);
int pk_text_width(const char *s);
int pk_text_width_scaled(const char *s, int scale);

// 画一行（混排：ASCII 半宽 + CJK 全角）。返回结束画笔 x。
int pk_draw_text(int x, int y, const char *s, Color color);
int pk_draw_text_scaled(int x, int y, const char *s, Color color, int scale);

// 解析 UTF-8 一个码点。返回字节数，*cp 写出；非法按 1 字节处理。
int pk_utf8_next(const char *s, uint16_t *cp);

// ---------------------------------------------------------------------------
// 2bpp 精灵（span 绘制，3=透明）
// ---------------------------------------------------------------------------

// palette 是 4 个 RGB565 值（来自 assets_palette）。
void pk_draw_sprite_2bpp(int x, int y, const uint8_t *data, int w, int h,
                         int scale, const uint16_t *palette);

// 缩到 dest_size 的缩略图（图鉴网格用）。
void pk_draw_sprite_thumbnail(int x, int y, const uint8_t *data, int size,
                              int dest_size, const uint16_t *palette);

// RGB565 → raylib Color。
static inline Color pk_rgb565_to_color(uint16_t v)
{
    Color c;
    c.r = (unsigned char)((((v >> 11) & 0x1F) * 255 + 15) / 31);
    c.g = (unsigned char)((((v >> 5) & 0x3F) * 255 + 31) / 63);
    c.b = (unsigned char)(((v & 0x1F) * 255 + 15) / 31);
    c.a = 255;
    return c;
}

// ---------------------------------------------------------------------------
// 通用控件（触摸热区与绘制共用同一份几何）
// ---------------------------------------------------------------------------

typedef struct {
    int x, y, w, h;
} pk_rect_t;

static inline bool pk_hit(const pk_rect_t *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// 中文按钮（字模居中）。
void pk_draw_button(const pk_rect_t *r, const char *label, Color bg,
                    Color text, bool pressed);

// HP 条：0~max，绿色 → 黄 → 红。
void pk_draw_hp_bar(int x, int y, int w, int h, int hp, int max_hp);

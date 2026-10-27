// pk/pk_draw.c —— 见 pk_draw.h。
#include <string.h>

#include "pk_draw.h"
#include "pk_assets.h"
#include "pk_port.h"

static const char *TAG = "pk_draw";

// ---------------------------------------------------------------------------
// FNT1 字库（自原版 render.c 移植）
// 格式：头 14B "FNT1"|ver u16|size u16|per u16|count u32；码点 u16 升序；
//       字形 count×per，1bpp 每行 size/8 字节，高位在左。
// ---------------------------------------------------------------------------

static struct {
    const uint16_t *codes;
    const uint8_t *glyphs;
    uint16_t size;
    uint16_t per;
    uint32_t count;
    bool ok;
} s_font;

bool pk_font_init(void)
{
    const pk_blob_t *blob = pk_asset("font16");
    if (!blob || blob->size < 14) {
        PK_LOGE(TAG, "font16.bin missing or truncated");
        return false;
    }
    const uint8_t *d = blob->data;
    if (memcmp(d, "FNT1", 4) != 0) {
        PK_LOGE(TAG, "font16.bin magic mismatch");
        return false;
    }
    uint16_t size = (uint16_t)(d[6] | (d[7] << 8));
    uint16_t per = (uint16_t)(d[8] | (d[9] << 8));
    uint32_t cnt = (uint32_t)d[10] | ((uint32_t)d[11] << 8) |
                   ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
    if (14u + cnt * 2 + cnt * per != blob->size) {
        PK_LOGE(TAG, "font16.bin length mismatch: 14+%u*2+%u*%u != %u",
                (unsigned)cnt, (unsigned)cnt, per, (unsigned)blob->size);
        return false;
    }
    s_font.codes = (const uint16_t *)(const void *)(d + 14);
    s_font.glyphs = d + 14 + cnt * 2;
    s_font.size = size;
    s_font.per = per;
    s_font.count = cnt;
    s_font.ok = true;
    return true;
}

uint16_t pk_font_size(void)
{
    return s_font.ok ? s_font.size : 16;
}

int pk_utf8_next(const char *s, uint16_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0) {
        *cp = (uint16_t)(((u[0] & 0x1F) << 6) | (u[1] & 0x3F));
        return 2;
    }
    if ((u[0] & 0xF0) == 0xE0) {
        *cp = (uint16_t)(((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) |
                         (u[2] & 0x3F));
        return 3;
    }
    *cp = u[0];
    return 1;
}

// 二分查码点 → 字形下标。码点索引升序（转换脚本保证）。
static int glyph_index(uint16_t cp)
{
    if (!s_font.ok) return -1;
    int lo = 0, hi = (int)s_font.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = s_font.codes[mid];
        if (c == cp) return mid;
        if (c < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

uint8_t pk_char_advance(uint16_t cp)
{
    // ASCII 半宽：font16 是定长 16×16，但 ASCII 墨迹落在 x=1~13，
    // 按 8px 步进只切掉右侧空白（原版实测结论，沿用）。
    return (cp < 0x80) ? (uint8_t)(s_font.size / 2) : (uint8_t)s_font.size;
}

int pk_text_width(const char *s)
{
    return pk_text_width_scaled(s, 1);
}

int pk_text_width_scaled(const char *s, int scale)
{
    if (!s) return 0;
    if (scale < 1) scale = 1;
    int w = 0;
    while (*s) {
        uint16_t cp;
        s += pk_utf8_next(s, &cp);
        if (glyph_index(cp) >= 0) w += pk_char_advance(cp) * scale;
    }
    return w;
}

// 画一个字形：按行找墨迹跨度，每段一个 DrawRectangle。scale>1 时像素放大。
static void draw_glyph(int gi, int x, int y, Color color, int scale)
{
    const uint8_t *g = s_font.glyphs + (size_t)gi * s_font.per;
    const int row_bytes = (s_font.size + 7) / 8;
    for (uint16_t r = 0; r < s_font.size; r++) {
        int run_start = -1;
        for (uint16_t c = 0; c <= s_font.size; c++) {
            int ink = 0;
            if (c < s_font.size) {
                ink = (g[r * row_bytes + c / 8] >> (7 - (c % 8))) & 1;
            }
            if (ink && run_start < 0) {
                run_start = c;
            } else if (!ink && run_start >= 0) {
                DrawRectangle(x + run_start * scale, y + r * scale,
                              (c - run_start) * scale, scale, color);
                run_start = -1;
            }
        }
    }
}

int pk_draw_text_scaled(int x, int y, const char *s, Color color, int scale)
{
    if (!s_font.ok || !s) return x;
    if (scale < 1) scale = 1;
    while (*s) {
        uint16_t cp;
        s += pk_utf8_next(s, &cp);
        int gi = glyph_index(cp);
        if (gi >= 0) {
            // ASCII 半宽：把字形左移 size/4 让墨迹落在 8px 步进中间。
            int dx = (cp < 0x80) ? -(int)(s_font.size / 4) * scale : 0;
            draw_glyph(gi, x + dx, y, color, scale);
        }
        x += pk_char_advance(cp) * scale;
    }
    return x;
}

int pk_draw_text(int x, int y, const char *s, Color color)
{
    return pk_draw_text_scaled(x, y, s, color, 1);
}

// ---------------------------------------------------------------------------
// 2bpp 精灵（3 = 透明）
// ---------------------------------------------------------------------------

void pk_draw_sprite_2bpp(int x, int y, const uint8_t *data, int w, int h,
                         int scale, const uint16_t *palette)
{
    if (!data || !palette || w <= 0 || h <= 0 || scale <= 0) return;
    const int row_bytes = (w * 2 + 7) / 8;
    for (int sy = 0; sy < h; sy++) {
        const uint8_t *row = data + sy * row_bytes;
        int run_start = -1;
        uint8_t run_shade = 0;
        for (int sx = 0; sx <= w; sx++) {
            int shade = 3;
            if (sx < w) {
                shade = (row[(sx * 2) / 8] >> (6 - (sx * 2) % 8)) & 3;
            }
            if (shade != 3 && run_start < 0) {
                run_start = sx;
                run_shade = (uint8_t)shade;
            } else if (shade == 3 && run_start >= 0) {
                DrawRectangle(x + run_start * scale, y + sy * scale,
                              (sx - run_start) * scale, scale,
                              pk_rgb565_to_color(palette[run_shade]));
                run_start = -1;
            } else if (shade != run_shade && run_start >= 0) {
                DrawRectangle(x + run_start * scale, y + sy * scale,
                              (sx - run_start) * scale, scale,
                              pk_rgb565_to_color(palette[run_shade]));
                run_start = sx;
                run_shade = (uint8_t)shade;
            }
        }
    }
}

void pk_draw_sprite_thumbnail(int x, int y, const uint8_t *data, int size,
                              int dest_size, const uint16_t *palette)
{
    if (!data || !palette || size <= 0 || dest_size <= 0 || dest_size > size) return;
    const int row_bytes = (size + 3) / 4;
    for (int dy = 0; dy < dest_size; dy++) {
        int sy = ((2 * dy + 1) * size) / (2 * dest_size);
        const uint8_t *row = data + sy * row_bytes;
        for (int dx = 0; dx < dest_size; dx++) {
            int sx = ((2 * dx + 1) * size) / (2 * dest_size);
            uint8_t shade = (row[sx / 4] >> (6 - 2 * (sx % 4))) & 3;
            if (shade != 3) {
                DrawRectangle(x + dx, y + dy, 1, 1,
                              pk_rgb565_to_color(palette[shade]));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 控件
// ---------------------------------------------------------------------------

void pk_draw_button(const pk_rect_t *r, const char *label, Color bg,
                    Color text, bool pressed)
{
    (void)pressed;
    DrawRectangle(r->x, r->y, r->w, r->h, bg);
    DrawRectangleLines(r->x, r->y, r->w, r->h, (Color){ 40, 30, 15, 255 });
    if (!label) return;
    const int fs = pk_font_size();
    const int tw = pk_text_width(label);
    pk_draw_text(r->x + (r->w - tw) / 2, r->y + (r->h - fs) / 2, label, text);
}

void pk_draw_hp_bar(int x, int y, int w, int h, int hp, int max_hp)
{
    DrawRectangle(x, y, w, h, (Color){ 45, 45, 45, 255 });
    if (max_hp <= 0) return;
    if (hp < 0) hp = 0;
    if (hp > max_hp) hp = max_hp;
    const int fill = w * hp / max_hp;
    Color c = (hp * 100 >= max_hp * 50) ? (Color){ 110, 200, 90, 255 }
            : (hp * 100 >= max_hp * 20) ? (Color){ 230, 190, 70, 255 }
                                        : (Color){ 220, 80, 70, 255 };
    if (fill > 0) DrawRectangle(x, y, fill, h, c);
    DrawRectangleLines(x, y, w, h, (Color){ 40, 30, 15, 255 });
}

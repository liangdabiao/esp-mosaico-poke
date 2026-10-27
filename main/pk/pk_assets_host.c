// pk/pk_assets_host.c —— 宿主模拟器的资产来源：运行时从磁盘读。
//
// 设备端资产链在 flash 里；host 构建没有那些符号，这里在首次
// pk_asset() 时把 main/assets/*.bin 读进静态缓冲。搜索顺序：
//   1. pk_assets_set_root() 设置的根（game_module.c 传入 asset_root）
//   2. 环境变量 PKGO_ASSETS
//   3. 常见相对位置兜底
// 找不到时返回 NULL，assets_init 会整体失败 —— 模拟器日志会报出来。
#include "pk_assets.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PK_HOST_ASSET_COUNT 7

static const char *NAMES[PK_HOST_ASSET_COUNT] = {
    "gen1", "gen1_front", "gen1_back", "palettes", "ui", "moves", "font16",
};

static struct {
    pk_blob_t blob;
    bool tried;
} s_cache[PK_HOST_ASSET_COUNT];

/* 最大的资产是 gen1_front.bin（约 90KB）与 font16.bin（约 42KB），128KB 足够。 */
static uint8_t s_buffers[PK_HOST_ASSET_COUNT][1 << 17];

/* 根目录。game_module.c 的宿主 initialize() 会调用 pk_assets_set_root()。 */
static char s_roots[8][512];
static int s_root_count;

void pk_assets_set_root(const char *root)
{
    if (!root || !*root || s_root_count >= 8) return;
    /* 去重：同一根只记一次。 */
    for (int i = 0; i < s_root_count; i++) {
        if (strcmp(s_roots[i], root) == 0) return;
    }
    snprintf(s_roots[s_root_count++], sizeof(s_roots[0]), "%s", root);
}

const pk_blob_t *pk_asset(const char *name)
{
    int idx = -1;
    for (int i = 0; i < PK_HOST_ASSET_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) { idx = i; break; }
    }
    if (idx < 0) return NULL;
    if (s_cache[idx].tried) return s_cache[idx].blob.data ? &s_cache[idx].blob : NULL;
    s_cache[idx].tried = true;

    /* 组装候选根目录（env + set_root + 相对位置兜底），逐个尝试。 */
    char roots[12][512];
    int root_count = 0;
    const char *env = getenv("PKGO_ASSETS");
    if (env && *env && strlen(env) < sizeof(roots[0]))
        snprintf(roots[root_count++], sizeof(roots[0]), "%s", env);
    for (int i = 0; i < s_root_count && root_count < 12; i++)
        strcpy(roots[root_count++], s_roots[i]);
    static const char *FALLBACKS[] = {
        "main/assets", "../main/assets", "projects/pokego/main/assets",
    };
    for (size_t i = 0; i < sizeof(FALLBACKS) / sizeof(FALLBACKS[0]) && root_count < 12; i++)
        snprintf(roots[root_count++], sizeof(roots[0]), "%s", FALLBACKS[i]);

    for (int r = 0; r < root_count; r++) {
        /* set_root 传来的可能是项目根（内含 main/assets）也可能是 assets 本身。 */
        static const char *SUB[] = { "", "/main/assets" };
        for (size_t s = 0; s < sizeof(SUB) / sizeof(SUB[0]); s++) {
            char path[1100];
            snprintf(path, sizeof(path), "%-.510s%-.12s/%s.bin", roots[r], SUB[s],
                     NAMES[idx]);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            size_t n = fread(s_buffers[idx], 1, sizeof(s_buffers[idx]), f);
            fclose(f);
            if (n == 0 || n >= sizeof(s_buffers[idx])) continue; /* 空文件或超缓冲 */
            s_cache[idx].blob.data = s_buffers[idx];
            s_cache[idx].blob.size = n;
            return &s_cache[idx].blob;
        }
    }
    printf("E pk_assets_host: cannot load %s.bin (tried %d roots)\n", NAMES[idx], root_count);
    return NULL;
}

// pk/pk_assets.h —— 资产字节来源的两端分叉。
//
// 设备端：7 个 .bin 用 target_add_binary_data 链进 flash（pk_assets_device.c
// 提供符号）；宿主模拟器没有链接期嵌入，pk_assets_host.c 从磁盘读进缓冲。
// 解析代码（pk/assets.c）只认 pk_asset()，不关心字节从哪来。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    size_t size;
} pk_blob_t;

// 按名字取资产字节。name 不带 .bin 后缀："gen1" / "gen1_front" /
// "gen1_back" / "palettes" / "ui" / "moves" / "font16"。
// 找不到返回 NULL（assets_init 会整体失败并置空查询）。
const pk_blob_t *pk_asset(const char *name);

// 宿主专用：登记资产搜索根目录（可多次调用，都会被尝试）。
// 设备端是空实现（assets 已链进 flash）。
void pk_assets_set_root(const char *root);

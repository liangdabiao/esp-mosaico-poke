// pk/pk_port.h —— 移植层的公共小件：日志宏与 host/device 差异。
//
// 原版（ESP32-PokemonGo）直接用 ESP_LOGx 与 esp_rom_crc32_le；
// 本项目要同时跑在宿主模拟器（无 ESP-IDF 头文件）与设备上，所以
// 统一在这里分叉：宿主走 printf，设备走 ESP_LOGx。游戏代码只认 PK_LOGx。
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef MOSAICO_HOST_SIMULATION
#include <stdio.h>
/* host 与设备同签名（tag, fmt, ...)：host 丢弃 tag，调用点不用分叉。 */
#define PK_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define PK_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define PK_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#else
#include "esp_log.h"
#define PK_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define PK_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define PK_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#endif

// 统一的 CRC-32（标准 CRC-32/IEEE，与 zlib.crc32、esp_rom_crc32_le 一致）。
// 原版在 host/device 各 include 一份实现；这里两端用同一份纯 C 打表实现，
// 保证「同一字节串得到同一种子」在两端可复现（回放/对账依赖它）。
uint32_t pk_crc32(const void *data, size_t len);

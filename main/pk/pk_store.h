// pk/pk_store.h —— 存档落盘（NVS / 宿主内存）。
//
// 与 weiqi-quest 的 wq_store 同一模式：格式校验在纯逻辑层
// （pk_world_deserialize），这里只搬字节 —— 写盘代码越薄，掉电时
// 可能出错的地方越少。
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t pk_store_init(void);

// 读存档到 buf。首次开机（没有记录）返回 ESP_OK 且 *len=0。
esp_err_t pk_store_load(uint8_t *buf, uint32_t cap, uint32_t *len);

// 写存档，写入后立即 commit。
esp_err_t pk_store_save(const uint8_t *buf, uint32_t len);

esp_err_t pk_store_erase(void);

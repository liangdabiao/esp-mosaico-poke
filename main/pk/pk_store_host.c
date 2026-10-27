// pk/pk_store_host.c —— 宿主模拟器的存档桩（进程内存「NVS」）。
// 语义与真机一致：load 读当前档、save 写回 —— 一次 sim 运行内
// 「开档 → 遇敌 → 捕获 → 保存 → 重置重读」这条链可以被复现。
// 该文件只出现在 game.sim.json（host），不进设备 CMakeLists。
#include "pk_store.h"
#include "pk_world.h"

#include <string.h>

static uint8_t s_memory[PK_SAVE_BLOB_SIZE + 64];
static uint32_t s_len;
static bool s_ready;

esp_err_t pk_store_init(void)
{
    s_ready = true;
    return ESP_OK;
}

esp_err_t pk_store_load(uint8_t *buf, uint32_t cap, uint32_t *len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;
    *len = 0;
    if (!s_ready) return ESP_FAIL;
    if (s_len == 0) return ESP_OK;
    if (cap < s_len) return ESP_ERR_NO_MEM;
    memcpy(buf, s_memory, s_len);
    *len = s_len;
    return ESP_OK;
}

esp_err_t pk_store_save(const uint8_t *buf, uint32_t len)
{
    if (!s_ready || !buf || !len || len > sizeof(s_memory)) return ESP_FAIL;
    memcpy(s_memory, buf, len);
    s_len = len;
    return ESP_OK;
}

esp_err_t pk_store_erase(void)
{
    s_len = 0;
    return ESP_OK;
}

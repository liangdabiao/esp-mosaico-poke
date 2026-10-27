// pk/crc32.c —— 标准 CRC-32/IEEE，两端同一份实现（见 pk_port.h）。
#include "pk_port.h"

static uint32_t table[256];
static bool table_ready;

static void table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    table_ready = true;
}

uint32_t pk_crc32(const void *data, size_t len)
{
    if (!table_ready) table_init();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

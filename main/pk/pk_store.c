// pk/pk_store.c —— 设备端存档：NVS blob（见 pk_store.h）。
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "pk_store.h"

static const char *TAG = "pk_store";
static const char *NAMESPACE = "pokego";
static const char *KEY_SAVE = "save";

esp_err_t pk_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs rebuild (%s); erasing and retrying", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t pk_store_load(uint8_t *buf, uint32_t cap, uint32_t *len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;
    *len = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t size = cap;
    err = nvs_get_blob(handle, KEY_SAVE, buf, &size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   // 首次开机：正常路径，len=0
    }
    if (err == ESP_OK) *len = (uint32_t)size;
    return err;
}

esp_err_t pk_store_save(const uint8_t *buf, uint32_t len)
{
    if (!buf || !len) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(handle, KEY_SAVE, buf, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t pk_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

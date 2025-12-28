#include "nvs_config.h"
#include "common_log.h"
#include <cstring>

#define TAG "NVS"

static nvs_handle_t s_nvs_handle;

void nvs_init_custom(const char *nvs_namespace) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open(nvs_namespace, NVS_READWRITE, &s_nvs_handle));
}

void nvs_read_config(nvs_config_t *config) {
    size_t ssid_len = sizeof(config->ssid);
    size_t psk_len = sizeof(config->psk);
    size_t hash_len = sizeof(config->ota_hash);
    config->method = 0; 
    memset(config->ssid, 0, 32);
    memset(config->psk, 0, 64);

    if(nvs_get_u8(s_nvs_handle, "method", &config->method) != ESP_OK) INFO("No method found");
    nvs_get_str(s_nvs_handle, "ssid", config->ssid, &ssid_len);
    nvs_get_str(s_nvs_handle, "psk", config->psk, &psk_len);
    nvs_get_blob(s_nvs_handle, "ota_hash", config->ota_hash, &hash_len);
    nvs_set_u8(s_nvs_handle, "updated", 0);
    nvs_commit(s_nvs_handle);
}

void nvs_mark_updated() {
    ESP_ERROR_CHECK(nvs_set_u8(s_nvs_handle, "updated", 1));
    ESP_ERROR_CHECK(nvs_commit(s_nvs_handle));
    nvs_close(s_nvs_handle);
}

void nvs_reset_meshtastic_counter() {
    nvs_handle_t mesh_nvs;
    if (nvs_open("meshtastic", NVS_READWRITE, &mesh_nvs) == ESP_OK) {
        nvs_set_u32(mesh_nvs, "rebootCounter", 0);
        nvs_commit(mesh_nvs);
        nvs_close(mesh_nvs);
    }
}

void nvs_get_meshtastic_info(uint32_t *reboot_counter, uint8_t *hw_vendor, char *fw_rev, size_t fw_rev_len) {
    nvs_handle_t mesh_handle;
    *reboot_counter = 0;
    *hw_vendor = 0;
    memset(fw_rev, 0, fw_rev_len);

    if (nvs_open("meshtastic", NVS_READONLY, &mesh_handle) == ESP_OK) {
        nvs_get_u32(mesh_handle, "rebootCounter", reboot_counter);
        nvs_get_u8(mesh_handle, "hwVendor", hw_vendor);
        nvs_get_str(mesh_handle, "firmwareVersion", fw_rev, &fw_rev_len);
        nvs_close(mesh_handle);
    }
}

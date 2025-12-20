#pragma once
#include <stdint.h>
#include "nvs_flash.h"
#include "nvs.h"

typedef struct {
    uint8_t method;
    char ssid[32];
    char psk[64];
} wifi_credentials_t;

void nvs_init_custom(const char *nvs_namespace);
void nvs_read_config(wifi_credentials_t *config);
void nvs_mark_updated();
void nvs_reset_meshtastic_counter();
void nvs_get_meshtastic_info(uint32_t *reboot_counter, uint8_t *hw_vendor, char *fw_rev, size_t fw_rev_len);

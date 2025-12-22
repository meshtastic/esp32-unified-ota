#include "net_ota.h"
#include "common_log.h"
#include "nvs_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "mbedtls/md5.h"
#include <cstring>
#include <cstdio>

#define TAG "NET_OTA"
#define OK "OK"
#define OK_LEN 2
static char buffer[1024];

typedef enum { ESPOTA_CMD_FLASH = 0, ESPOTA_CMD_SPIFFS = 100 } espota_cmd_t;

typedef struct {
    in_port_t remote_port;
    struct sockaddr_in remote_ctrl_addr;
    size_t firmware_size;
    uint8_t firmware_md5[16];
    espota_cmd_t cmd;
} ota_config_t;

static bool md5_string_to_bytes(const char *hex, uint8_t *bytes) {
    for (int i = 0; i < 16; ++i) {
        unsigned int byte = 0;
        if (sscanf(&hex[i * 2], "%02x", &byte) != 1) return false;
        bytes[i] = (uint8_t)byte;
    }
    return true;
}

static void ota_parse_config(const char *buffer, ota_config_t *config) {
    int command = 0;
    unsigned int remote_port = 0;
    unsigned int firmware_size = 0;
    char md5[33] = { 0 };
    int res = sscanf(buffer, "%d %u %u %32s", &command, &remote_port, &firmware_size, md5);
    if (res != 4) FAIL("Invalid header format");
    config->cmd = (espota_cmd_t)command;
    config->remote_port = htons(remote_port);
    config->firmware_size = firmware_size;
    uint8_t md5_bytes[16];
    if (!md5_string_to_bytes(md5, md5_bytes)) FAIL("Invalid MD5 string");
    memcpy(config->firmware_md5, md5_bytes, sizeof(md5_bytes));
}

void start_network_ota_process() {
    INFO("Waiting for OTA invitation on port 3232");
    
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) FAIL("Failed to create UDP socket");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(3232);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) FAIL("UDP Bind failed");
    
    struct sockaddr_in source_addr;
    socklen_t source_len = sizeof(source_addr);
    
    struct timeval timeout = { .tv_sec = 120, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Reserve 1 byte for null terminator so sscanf doesn't overflow
    int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&source_addr, &source_len);
    if (len < 0) FAIL("Timeout waiting for OTA");
    
    // Null terminate buffer for string parsing
    buffer[len] = 0; 

    ota_config_t config;
    config.remote_ctrl_addr = source_addr;
    ota_parse_config(buffer, &config);

    const esp_partition_t *part_target = NULL;
    esp_ota_handle_t update_handle = 0;

    if (config.cmd == ESPOTA_CMD_FLASH) {
        // PER REQUEST: Always targeting OTA_0
        part_target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!part_target) FAIL("OTA_0 partition not found");

        INFO("Starting OTA on partition subtype %d at offset 0x%lx", part_target->subtype, part_target->address);

        // Confirm invitation (UDP ACK)
        sendto(sock, "ERASE", 5, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));

        // Safety: Set boot partition to current before starting, in case of power loss
        // Think about when we want to lock the user into this bootloader.
        // Once the erase starts, there's no going back!
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_set_boot_partition(running);

        // esp_ota_begin handles erasing based on config.firmware_size
        if (esp_ota_begin(part_target, config.firmware_size, &update_handle) != ESP_OK) {
            FAIL("esp_ota_begin failed");
        }
    } else {
        // SPIFFS/LittleFS UPDATE
        // Note: We cannot use esp_ota_begin/write for SPIFFS because esp_ota validates 
        // app image headers which file systems do not have. We stick to partition ops here.
        part_target = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
        if (!part_target) FAIL("SPIFFS partition not found");

        INFO("Erasing SPIFFS partition...");
        esp_partition_erase_range(part_target, 0, part_target->size);
    }

    // Connect Data TCP Socket
    int data_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in data_addr = source_addr;
    data_addr.sin_port = config.remote_port;
    
    // Confirm invitation (UDP ACK)
    sendto(sock, OK, OK_LEN, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        FAIL("Connect failed");
    }
    closesocket(sock); // Close UDP control

    // MbedTLS 3.x / IDF 5.x syntax
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0) { // No _ret in MbedTLS 3
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        FAIL("MD5 init failed");
    }

    size_t total = 0;
    while (total < config.firmware_size) {
        size_t want = config.firmware_size - total;
        if (want > sizeof(buffer)) want = sizeof(buffer);

        int r = recv(data_sock, buffer, want, 0);
        if (r <= 0) break;
        
        // Update hash
        if (mbedtls_md5_update(&ctx, (unsigned char*)buffer, r) != 0) {
            if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
            FAIL("MD5 update failed");
        }
        
        // Write to flash
        esp_err_t write_err = ESP_OK;
        if (config.cmd == ESPOTA_CMD_FLASH) {
            // Use OTA API for App
            write_err = esp_ota_write(update_handle, buffer, r);
        } else {
            // Use Partition API for SPIFFS
            write_err = esp_partition_write(part_target, total, buffer, r);
        }

        if (write_err != ESP_OK) {
             if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
             FAIL("Flash write failed");
        }
        
        total += r;
    }

    if (total != config.firmware_size) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        FAIL("Incomplete firmware image %d of %d", total, config.firmware_size);
    }
    
    uint8_t digest[16];
    if (mbedtls_md5_finish(&ctx, digest) != 0) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        FAIL("MD5 finish failed");
    }
    mbedtls_md5_free(&ctx); 
    
    // Compare Hash
    if (memcmp(digest, config.firmware_md5, 16) != 0) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        printf("Calc: ");
        for(int i=0; i<16; i++) printf("%02x", digest[i]);
        printf("\nExp : ");
        for(int i=0; i<16; i++) printf("%02x", config.firmware_md5[i]);
        printf("\n");
        FAIL("MD5 Mismatch");
    }
    
    if (config.cmd == ESPOTA_CMD_FLASH) {
        // Validates image (checks magic byte 0xE9 and checksum)
        if (esp_ota_end(update_handle) != ESP_OK) {
            FAIL("OTA End/Validation failed");
        }

        // Reset Meshtastic reboot counter
        INFO("Resetting Meshtastic reboot counter...");
        nvs_handle_t mesh_nvs;
        if (nvs_open("meshtastic", NVS_READWRITE, &mesh_nvs) == ESP_OK) {
            nvs_set_u32(mesh_nvs, "rebootCounter", 0);
            nvs_commit(mesh_nvs);
            nvs_close(mesh_nvs);
        }

        // Set boot partition to OTA_0 (part_target)
        if (esp_ota_set_boot_partition(part_target) != ESP_OK) {
            FAIL("Set boot partition failed");
        }
        INFO("OTA Updated. Next boot on OTA_0.");
    } else {
        INFO("SPIFFS Updated.");
    }
    
    closesocket(data_sock);
}

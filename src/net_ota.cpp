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
#include "mbedtls/sha256.h"
#include <cstring>
#include <cstdio>
#include "utils.h"

#define TAG "NET_OTA"
#define OK "OK"
#define OK_LEN 2
static char buffer[1024];

typedef enum { ESPOTA_CMD_FLASH = 0, ESPOTA_CMD_SPIFFS = 100 } espota_cmd_t;

typedef struct {
    in_port_t remote_port;
    struct sockaddr_in remote_ctrl_addr;
    size_t firmware_size;
    uint8_t firmware_hash[32];  // Full 32-byte SHA-256 hash
    espota_cmd_t cmd;
} ota_config_t;

static bool hash_string_to_bytes(const char *hex, uint8_t *bytes) {
    for (int i = 0; i < 32; ++i) {  // Now 32 bytes instead of 16
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
    char hash_hex[65] = { 0 };  // 64 hex chars + null terminator
    int res = sscanf(buffer, "%d %u %u %64s", &command, &remote_port, &firmware_size, hash_hex);
    if (res != 4) FAIL("Invalid header format");
    config->cmd = (espota_cmd_t)command;
    config->remote_port = htons(remote_port);
    config->firmware_size = firmware_size;
    uint8_t hash_bytes[32];
    if (!hash_string_to_bytes(hash_hex, hash_bytes)) FAIL("Invalid SHA-256 hash string");
    memcpy(config->firmware_hash, hash_bytes, sizeof(hash_bytes));
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

    int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&source_addr, &source_len);
    if (len < 0) FAIL("Timeout waiting for OTA");
    
    buffer[len] = 0; 

    ota_config_t config;
    config.remote_ctrl_addr = source_addr;
    ota_parse_config(buffer, &config);

    const esp_partition_t *part_target = NULL;
    esp_ota_handle_t update_handle = 0;

    if (config.cmd == ESPOTA_CMD_FLASH) {
        part_target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        if (!part_target) FAIL("OTA_0 partition not found");

        INFO("Starting OTA on partition subtype %d at offset 0x%lx", part_target->subtype, part_target->address);

        sendto(sock, "ERASE", 5, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));

        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_set_boot_partition(running);

        if (esp_ota_begin(part_target, config.firmware_size, &update_handle) != ESP_OK) {
            FAIL("esp_ota_begin failed");
        }
    } else {
        part_target = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
        if (!part_target) FAIL("SPIFFS partition not found");

        INFO("Erasing SPIFFS partition...");
        esp_partition_erase_range(part_target, 0, part_target->size);
    }

    int data_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in data_addr = source_addr;
    data_addr.sin_port = config.remote_port;
    
    sendto(sock, OK, OK_LEN, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        FAIL("Connect failed");
    }
    closesocket(sock);

    // SHA-256 context
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {  // 0 = SHA-256
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        FAIL("SHA-256 init failed");
    }

    size_t total = 0;
    while (total < config.firmware_size) {
        size_t want = config.firmware_size - total;
        if (want > sizeof(buffer)) want = sizeof(buffer);

        int r = recv(data_sock, buffer, want, 0);
        if (r <= 0) break;
        
        // Update SHA-256 hash
        if (mbedtls_sha256_update(&ctx, (unsigned char*)buffer, r) != 0) {
            if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
            FAIL("SHA-256 update failed");
        }
        
        // Write to flash
        esp_err_t write_err = ESP_OK;
        if (config.cmd == ESPOTA_CMD_FLASH) {
            write_err = esp_ota_write(update_handle, buffer, r);
        } else {
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
        corrupt_partition(part_target);
        FAIL("Incomplete firmware image %d of %d", total, config.firmware_size);
        return;
    }
    
    uint8_t digest[32];
    if (mbedtls_sha256_finish(&ctx, digest) != 0) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        corrupt_partition(part_target);
        FAIL("SHA-256 finish failed");
        return;
    }
    mbedtls_sha256_free(&ctx); 
    
    // Compare full 32-byte SHA-256 hash
    if (memcmp(digest, config.firmware_hash, 32) != 0) {
        if (config.cmd == ESPOTA_CMD_FLASH) esp_ota_abort(update_handle);
        printf("Calc: ");
        for(int i=0; i<32; i++) printf("%02x", digest[i]);
        printf("\nExp : ");
        for(int i=0; i<32; i++) printf("%02x", config.firmware_hash[i]);
        printf("\n");
        FAIL("SHA-256 Mismatch");
    }
    
    if (config.cmd == ESPOTA_CMD_FLASH) {
        if (esp_ota_end(update_handle) != ESP_OK) {
            FAIL("OTA End/Validation failed");
        }

        INFO("Resetting Meshtastic reboot counter...");
        nvs_handle_t mesh_nvs;
        if (nvs_open("meshtastic", NVS_READWRITE, &mesh_nvs) == ESP_OK) {
            nvs_set_u32(mesh_nvs, "rebootCounter", 0);
            nvs_commit(mesh_nvs);
            nvs_close(mesh_nvs);
        }

        if (esp_ota_set_boot_partition(part_target) != ESP_OK) {
            FAIL("Set boot partition failed");
        }
        INFO("OTA Updated. Next boot on OTA_0.");
    } else {
        INFO("SPIFFS Updated.");
    }
    
    closesocket(data_sock);
}


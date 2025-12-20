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
    
    // Null terminate the buffer.  Without this, sscanf could read garbage at the end of the string -> Bad MD5.
    buffer[len] = 0; 

    ota_config_t config;
    config.remote_ctrl_addr = source_addr;
    ota_parse_config(buffer, &config);

    const esp_partition_t *part_target = NULL;
    if (config.cmd == ESPOTA_CMD_FLASH) {
        part_target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    } else {
        part_target = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    }
    if (!part_target) FAIL("Target partition not found");

    INFO("Erasing partition...");
    esp_partition_erase_range(part_target, 0, part_target->size);
    
    // Safety: Set boot partition to current before starting, in case of power loss
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_set_boot_partition(running);

    // Connect Data TCP Socket
    int data_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in data_addr = source_addr;
    data_addr.sin_port = config.remote_port;
    
    // Confirm invitation (UDP ACK)
    sendto(sock, OK, OK_LEN, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        FAIL("Connect failed");
    }
    closesocket(sock); // Close UDP control

    // FIX 3: MbedTLS 3.x (IDF 5.x) syntax
    // Functions return int (0 on success), no _ret suffix
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0) FAIL("MD5 init failed");

    size_t total = 0;
    while (total < config.firmware_size) {
        size_t want = config.firmware_size - total;
        if (want > sizeof(buffer)) want = sizeof(buffer);

        int r = recv(data_sock, buffer, want, 0);
        if (r <= 0) break;
        
        // Update hash
        if (mbedtls_md5_update(&ctx, (unsigned char*)buffer, r) != 0) FAIL("MD5 update failed");
        
        // Write to flash
        if (esp_partition_write(part_target, total, buffer, r) != ESP_OK) FAIL("Flash write failed");
        
        total += r;
    }

    if (total != config.firmware_size) FAIL("Incomplete firmware image %d of %d", total, config.firmware_size);
    
    uint8_t digest[16];
    if (mbedtls_md5_finish(&ctx, digest) != 0) FAIL("MD5 finish failed");
    mbedtls_md5_free(&ctx); 
    
    // Compare Hash
    if (memcmp(digest, config.firmware_md5, 16) != 0) {
        printf("Calc: ");
        for(int i=0; i<16; i++) printf("%02x", digest[i]);
        printf("\nExp : ");
        for(int i=0; i<16; i++) printf("%02x", config.firmware_md5[i]);
        printf("\n");
        FAIL("MD5 Mismatch");
    }
    
    if (config.cmd == ESPOTA_CMD_FLASH) {
        // FIX 4: Reset Meshtastic reboot counter
        INFO("Resetting Meshtastic reboot counter...");
        nvs_handle_t mesh_nvs;
        if (nvs_open("meshtastic", NVS_READWRITE, &mesh_nvs) == ESP_OK) {
            nvs_set_u32(mesh_nvs, "rebootCounter", 0);
            nvs_commit(mesh_nvs);
            nvs_close(mesh_nvs);
        }

        esp_ota_set_boot_partition(part_target);
    }
    
    closesocket(data_sock);
}

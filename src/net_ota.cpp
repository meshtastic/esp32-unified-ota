#include "net_ota.h"
#include "common_log.h"
#include "nvs_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/sha256.h"
#include <cstring>
#include <cstdio>
#include <string>
#include "utils.h"
#include "nvs_config.h"

#define TAG "NET_OTA"
#define OTA_PORT 3232
#define BROADCAST_INTERVAL_SEC 1

// Responses
#define RESP_OK "OK\n"
#define RESP_ERR "ERR\n"

static char buffer[1024];

typedef struct {
    size_t firmware_size;
    uint8_t firmware_hash[32];
    bool active;
} ota_context_t;

static bool hash_string_to_bytes(const char *hex, uint8_t *bytes) {
    for (int i = 0; i < 32; ++i) {
        unsigned int byte = 0;
        if (sscanf(&hex[i * 2], "%02x", &byte) != 1) return false;
        bytes[i] = (uint8_t)byte;
    }
    return true;
}

// --- Command Handlers ---

static void handle_version(int sock) {
    uint32_t reboot_counter = 0;
    uint8_t hw_vendor = 0;
    char fw_rev[32] = {0};
    
    nvs_get_meshtastic_info(&reboot_counter, &hw_vendor, fw_rev, sizeof(fw_rev));
    
    char resp[128];
    // Format: OK <hw_vendor> <fw_revision> <reboot_counter>
    snprintf(resp, sizeof(resp), "OK %d %s %lu\n", hw_vendor, fw_rev, (unsigned long)reboot_counter);
    send(sock, resp, strlen(resp), 0);
    INFO("Sent Version Info: %s", resp);
}

static void handle_reboot(int sock) {
    send(sock, RESP_OK, strlen(RESP_OK), 0);
    INFO("Reboot command received. Restarting...");
    vTaskDelay(500 / portTICK_PERIOD_MS); // Give time for ACK to flush
    esp_restart();
}

static bool handle_ota_cmd(int sock, const char* args, ota_context_t* ota_ctx) {
    unsigned int size = 0;
    char hash_hex[65] = {0};

    // Parse args: "<size> <hash>"
    if (sscanf(args, "%u %64s", &size, hash_hex) != 2) {
        send(sock, "ERR Invalid Format\n", 19, 0);
        return false;
    }

    if (!hash_string_to_bytes(hash_hex, ota_ctx->firmware_hash)) {
        send(sock, "ERR Invalid Hash\n", 17, 0);
        return false;
    }

    ota_ctx->firmware_size = size;
    ota_ctx->active = true;

    // Prepare Partition
    const esp_partition_t *part_target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!part_target) {
        send(sock, "ERR No Partition\n", 17, 0);
        return false;
    }

    INFO("Preparing OTA on partition subtype %d at offset 0x%lx", part_target->subtype, part_target->address);

    // Safety: Switch boot partition to current (running) one before erasing target
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_set_boot_partition(running);

    // Send OK to signal readiness for binary stream
    send(sock, RESP_OK, strlen(RESP_OK), 0);
    return true; // Break command loop
}

void start_network_ota_process(const nvs_config_t *nvs_config) {
    INFO("Starting Network Listener...");

    // 1. Setup UDP Broadcast (Discovery) - Done once
    int udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) FAIL("Failed to create UDP socket");

    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(OTA_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char discovery_msg[32];
    snprintf(discovery_msg, sizeof(discovery_msg), "MeshtasticOTA_%02x%02x%02x", mac[3], mac[4], mac[5]);

    // 2. Setup TCP Listener (Command/Data) - Done once
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) FAIL("Failed to create TCP socket");

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(OTA_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) FAIL("TCP Bind failed");
    if (listen(listen_sock, 1) < 0) FAIL("TCP Listen failed");

    INFO("Listening on TCP port %d", OTA_PORT);

    // --- Main Server Loop ---
    // We loop here forever (until reboot) to handle multiple connection attempts (e.g. probes)
    while (true) {
        int client_sock = -1;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        INFO("Waiting for client...");

        // 3. Discovery Loop: Broadcast until connected
        while (client_sock < 0) {
            sendto(udp_sock, discovery_msg, strlen(discovery_msg), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_sock, &readfds);

            struct timeval tv = { .tv_sec = BROADCAST_INTERVAL_SEC, .tv_usec = 0 };
            int activity = select(listen_sock + 1, &readfds, NULL, NULL, &tv);

            if (activity > 0 && FD_ISSET(listen_sock, &readfds)) {
                client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
                if (client_sock >= 0) INFO("Client connected from %s", inet_ntoa(client_addr.sin_addr));
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        // ota_ctx must be explicitly initialized to 0 to prevent warnings
        ota_context_t ota_ctx;
        memset(&ota_ctx, 0, sizeof(ota_context_t));
        
        bool keep_connection = true;

        // 4. Command Processing Loop
        while (keep_connection && !ota_ctx.active) {
            int len = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
            if (len <= 0) {
                INFO("Client disconnected or error");
                keep_connection = false;
                break;
            }
            buffer[len] = 0;

            // Strip newline
            char *newline = strchr(buffer, '\n');
            if (newline) *newline = 0;
            newline = strchr(buffer, '\r');
            if (newline) *newline = 0;

            INFO("Received Command: %s", buffer);

            if (strncmp(buffer, "VERSION", 7) == 0) {
                handle_version(client_sock);
            } else if (strncmp(buffer, "REBOOT", 6) == 0) {
                handle_reboot(client_sock);
                keep_connection = false;
            } else if (strncmp(buffer, "OTA", 3) == 0) {
                // "OTA <size> <hash>"
                if (handle_ota_cmd(client_sock, buffer + 4, &ota_ctx)) {
                    // OTA Started successfully, break loop to enter stream mode
                    break; 
                }
            } else {
                send(client_sock, "ERR Unknown Command\n", 20, 0);
            }
        }

        // 5. Binary OTA Stream Mode (Only if OTA command was successful)
        if (ota_ctx.active) {
            const esp_partition_t *part_target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
            esp_ota_handle_t update_handle = 0;

            if (esp_ota_begin(part_target, ota_ctx.firmware_size, &update_handle) != ESP_OK) {
                closesocket(client_sock);
                FAIL("OTA Begin Failed"); // Restart if critical failure
            }

            mbedtls_sha256_context sha_ctx;
            mbedtls_sha256_init(&sha_ctx);
            mbedtls_sha256_starts(&sha_ctx, 0); // 0 = SHA-256

            size_t total = 0;
            while (total < ota_ctx.firmware_size) {
                size_t want = ota_ctx.firmware_size - total;
                if (want > sizeof(buffer)) want = sizeof(buffer);

                int r = recv(client_sock, buffer, want, 0);
                if (r <= 0) break;

                mbedtls_sha256_update(&sha_ctx, (unsigned char*)buffer, r);
                
                if (esp_ota_write(update_handle, buffer, r) != ESP_OK) {
                    esp_ota_abort(update_handle);
                    closesocket(client_sock);
                    FAIL("Flash Write Failed");
                }
                total += r;
                if (total % 65536 == 0) INFO("Progress: %zu / %zu", total, ota_ctx.firmware_size);
            }

            uint8_t digest[32];
            mbedtls_sha256_finish(&sha_ctx, digest);
            mbedtls_sha256_free(&sha_ctx);

            if (total != ota_ctx.firmware_size) {
                esp_ota_abort(update_handle);
                corrupt_partition(part_target);
                INFO("Incomplete transfer. Resetting connection.");
                closesocket(client_sock);
                continue; // Loop back to discovery
            }

            if (memcmp(digest, ota_ctx.firmware_hash, 32) != 0) {
                esp_ota_abort(update_handle);
                print_hash("Calc: ", digest);
                print_hash("Exp : ", ota_ctx.firmware_hash);
                INFO("Hash Mismatch. Resetting connection.");
                closesocket(client_sock);
                continue; // Loop back to discovery
            }

            if (esp_ota_end(update_handle) != ESP_OK) {
                INFO("OTA End Failed. Resetting.");
                closesocket(client_sock);
                continue;
            }

            // Reset Counters
            nvs_handle_t mesh_nvs;
            if (nvs_open("meshtastic", NVS_READWRITE, &mesh_nvs) == ESP_OK) {
                nvs_set_u32(mesh_nvs, "rebootCounter", 0);
                nvs_commit(mesh_nvs);
                nvs_close(mesh_nvs);
            }

            if (esp_ota_set_boot_partition(part_target) == ESP_OK) {
                send(client_sock, RESP_OK, strlen(RESP_OK), 0);
                INFO("OTA Success. Rebooting.");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            } else {
                FAIL("Set Boot Partition Failed");
            }
        }

        // If we got here, either the client disconnected (probe) or an error occurred during OTA setup
        // Close the specific client socket and loop back to accept a new one.
        closesocket(client_sock);
        INFO("Session ended. Returning to listening state.");
    }
}

#include "net_ota.h"
#include "common_log.h"
#include "ota_processor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include <cstring>
#include "utils.h"

#define TAG "NET_OTA"
#define OTA_PORT 3232
#define BROADCAST_INTERVAL_SEC 1

void start_network_ota_process(const nvs_config_t *config) {
    INFO("Starting Network Listener...");

    int udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) FAIL("Failed to create UDP socket");
    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in broadcast_addr;
    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(OTA_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char discovery_msg[32];
    char* devName = getDeviceName();
    snprintf(discovery_msg, sizeof(discovery_msg), "%s %s", devName, GIT_VERSION);

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

    OtaProcessor otaProcessor;
    uint8_t rx_buffer[1024];

    // Set the expencted hash from the NVS
    otaProcessor.setNvramExpectedHash(config->ota_hash);

    while (true) {
        int client_sock = -1;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        INFO("Waiting for client...");
        
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

        otaProcessor.reset();
        otaProcessor.setSender([client_sock](const char* data, size_t len) {
            send(client_sock, data, len, 0);
        });

        while (true) {
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer), 0);
            if (len <= 0) {
                INFO("Client disconnected");
                break;
            }
            otaProcessor.process(rx_buffer, len);
            
            // Check for reboot request inside the loop or after recv returns
            // Note: process() is synchronous here.
            if (otaProcessor.isRebootRequired()) {
                INFO("Reboot flag detected. Restarting in 2 seconds...");
                vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait for ACK flush
                esp_restart();
            }
        }

        closesocket(client_sock);
        otaProcessor.reset();
    }
}

// 1. System Includes FIRST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"

// 2. Project Includes SECOND
#include "common_log.h"
#include "nvs_config.h"
#include "wifi_app.h"
#include "net_ota.h"
#include "ble_ota.h"

#define TAG "MAIN"

extern "C" void app_main(void) {

    
    nvs_init_custom("ota-wifi");
    INFO("\n\n//\\ E S H T /\\ S T / C OTA\n\n");
    INFO("Booting...");
    
    wifi_credentials_t config;
    nvs_read_config(&config);

    if (config.method == 1) {
        INFO("Mode: WiFi OTA");
        INFO("Connecting to SSID: %s", config.ssid);
        wifi_connect(&config);
        start_network_ota_process();
        INFO("Marking NVRAM as updated.");
        nvs_mark_updated();
        INFO("Success. Rebooting.");
        esp_restart();
    } else {
        INFO("Mode: BLE OTA");
        
        // Create the task with 8KB stack to prevent overflow
        xTaskCreate(ble_ota_task, "ble_ota_task", 8192, NULL, 5, NULL);
    }
}

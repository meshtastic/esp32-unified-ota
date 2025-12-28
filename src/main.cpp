// System Includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"

// Project Includes
#include "common_log.h"
#include "nvs_config.h"
#include "wifi_app.h"
#include "net_ota.h"
#include "ble_ota.h"
#include "utils.h"
#define TAG "MAIN"

#define NO_REBOOT_OTA 0
#define OTA_BLE 1
#define OTA_WIFI 2

extern "C" void app_main(void) {

    esp_netif_init();
    esp_event_loop_create_default();

    nvs_init_custom("MeshtasticOTA");

    INFO("\n\n//\\ E S H T /\\ S T / C\n\n");
    printf("OTA Loader");
    
    nvs_config_t config;
    nvs_read_config(&config);

    print_hash("Expecting firmware with hash: ", config.ota_hash);
    if (config.method == OTA_WIFI) {
        INFO("Mode: WiFi OTA");
        INFO("Connecting to SSID: %s", config.ssid);
        wifi_connect(&config);
        start_network_ota_process(&config);
        INFO("Marking NVRAM as updated.");
        nvs_mark_updated();
        INFO("Success. Rebooting.");
        esp_restart();
    } else {
        INFO("Mode: BLE OTA");
        
        // Create the task with 8KB stack to prevent overflow (might be uncessary)
        xTaskCreate(ble_ota_task, "ble_ota_task", 8192, NULL, 5, NULL);
    }
}


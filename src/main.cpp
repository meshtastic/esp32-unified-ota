// System Includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_app_desc.h"

// Project Includes
#include "common_log.h"
#include "nvs_config.h"
#include "wifi_app.h"
#include "net_ota.h"
// Only include BLE header if enabled
#ifdef USE_BLE_OTA
#include "ble_ota.h"
#endif
#include "utils.h"
#define TAG "MAIN"

#define NO_REBOOT_OTA 0
#define OTA_BLE 1
#define OTA_WIFI 2

extern "C" void app_main(void) {

    esp_netif_init();
    esp_event_loop_create_default();

    // Note: We keep the NVS namespace "MeshtasticOTA" constant so 
    // configuration is shared between WiFi-only and BLE variants.
    nvs_init_custom("MeshtasticOTA");
    
    const esp_app_desc_t *app_desc = esp_app_get_description();
    INFO("\n\n//\\ E S H T /\\ S T / C\n\n");
    // This will print "MeshtasticOTA" or "MeshtasticOTA-WiFi" based on CMake logic
    printf("OTA Loader v%s (%s)\r\n", app_desc->version, app_desc->project_name); 
    printf("Compiled on: %s %s\r\n", app_desc->date, app_desc->time);
    
    nvs_config_t config;
    nvs_read_config(&config);

    print_hash("Expecting firmware with hash: ", config.ota_hash);

    // If BLE is disabled at compile time, force WiFi mode or check for configuration errors
    #ifndef USE_BLE_OTA
    if (config.method == OTA_BLE) {
        INFO("Config requested BLE, but this firmware is WiFi-Only.");
        INFO("Defaulting to WiFi mode (verify credentials exist!).");
        config.method = OTA_WIFI;
    }
    #endif

    if (config.method == OTA_WIFI) {
        #ifdef USE_WIFI_OTA
        INFO("Mode: WiFi OTA");
        INFO("Connecting to SSID: %s", config.ssid);
        wifi_connect(&config);
        start_network_ota_process(&config);
        INFO("Marking NVRAM as updated.");
        nvs_mark_updated();
        INFO("Success. Rebooting.");
        esp_restart();
        #else
        FAIL("WiFi Mode requested but not compiled in.");
        #endif
    } else {
        #ifdef USE_BLE_OTA
        INFO("Mode: BLE OTA");
        // Create the task with 8KB stack to prevent overflow (might be uncessary)
        xTaskCreate(ble_ota_task, "ble_ota_task", 8192, NULL, 5, NULL);
        #else
        FAIL("BLE Mode requested but not compiled in.");
        #endif
    }
}


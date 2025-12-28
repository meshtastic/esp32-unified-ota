#include "wifi_app.h"
#include "common_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <cstring>

#define TAG "WIFI"
static const int wifi_connect_retries = 10;
static EventGroupHandle_t event_group_handle;
const int BIT_CONNECTED = BIT0;
const int BIT_FAIL = BIT1;

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int s_retry_num = 0;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < wifi_connect_retries) {
            esp_wifi_connect();
            s_retry_num++;
            INFO("Retry WiFi connect");
        } else {
            xEventGroupSetBits(event_group_handle, BIT_FAIL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        xEventGroupSetBits(event_group_handle, BIT_CONNECTED);
    }
}

void wifi_connect(const nvs_config_t *config) {
    event_group_handle = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);
    wifi_config_t wifi_config = {};
    strlcpy((char*)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char*)wifi_config.sta.password, config->psk, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(event_group_handle, BIT_CONNECTED | BIT_FAIL, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & BIT_FAIL) FAIL("Failed to connect to WiFi");
}

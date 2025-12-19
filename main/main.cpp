#include <string>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_mac.h"
#include "esp_random.h"

// Networking
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "mbedtls/md5.h"

// NimBLE (C++ Component)
#include "NimBLEDevice.h"

#define TAG "OTA"


// --- Macros for Logging ---
// --- Macros for Logging (Direct Printf) ---
// We use printf directly to bypass the disabled ESP_LOG system, which is disabled in the sdkconfig
// This keeps the binary small by not including library log strings.
#define INFO(format, ...) printf("I (%lu) %s: " format "\r\n", esp_log_timestamp(), TAG, ##__VA_ARGS__)
#define WARN(format, ...) printf("W (%lu) %s: " format "\r\n", esp_log_timestamp(), TAG, ##__VA_ARGS__)
#define FAIL(format, ...) do { printf("E (%lu) %s: " format "\r\n", esp_log_timestamp(), TAG, ##__VA_ARGS__); esp_restart(); } while (0)

#define OK "OK"
#define OK_LEN 2
static char buffer[1024];

// --- Config Structs ---
typedef struct {
    uint8_t method;
    char ssid[32];
    char psk[64];
} wifi_credentials_t;

// --- Global Variables ---
static nvs_handle_t s_nvs_handle;

// BLE
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
BLECharacteristic *pOtaCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define CHARACTERISTIC_TX_UUID "62ec0272-3ec5-11eb-b378-0242ac130003"
#define CHARACTERISTIC_OTA_UUID "62ec0272-3ec5-11eb-b378-0242ac130005"

// OTA Variables
static esp_ota_handle_t otaHandler = 0;
static const esp_partition_t *update_partition = NULL;
uint8_t txValue = 0;
int bufferCount = 0;
size_t ota_size = 0;
size_t total_received = 0;
uint8_t chunkCount = 1;
bool downloadFlag = false;

// --- NVS Helpers (Replacements for Preferences.h) ---
static void nvs_init_custom(const char *nvs_namespace) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    // Namespace: "ota-wifi" or "meshtastic"
    ESP_ERROR_CHECK(nvs_open(nvs_namespace, NVS_READWRITE, &s_nvs_handle));
}

static void nvs_read_config(wifi_credentials_t *config) {
    size_t ssid_len = sizeof(config->ssid);
    size_t psk_len = sizeof(config->psk);
    
    // Default values if not found
    config->method = 0; 
    memset(config->ssid, 0, 32);
    memset(config->psk, 0, 64);

    if(nvs_get_u8(s_nvs_handle, "method", &config->method) != ESP_OK) INFO("NVS: No method found");
    nvs_get_str(s_nvs_handle, "ssid", config->ssid, &ssid_len);
    nvs_get_str(s_nvs_handle, "psk", config->psk, &psk_len);
    
    // Reset updated flag on read
    nvs_set_u8(s_nvs_handle, "updated", 0);
    nvs_commit(s_nvs_handle);
}

static void nvs_mark_updated() {
    ESP_ERROR_CHECK(nvs_set_u8(s_nvs_handle, "updated", 1));
    ESP_ERROR_CHECK(nvs_commit(s_nvs_handle));
    nvs_close(s_nvs_handle);
}

// --- BLE Callbacks ---
class MyServerCallbacks : public BLEServerCallbacks {
    // NimBLE 2.x Signature: onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo)
    void onConnect(BLEServer *pServer, NimBLEConnInfo& connInfo) override {
        INFO("*** App connected");
        // Update connection params for speed
        pServer->updateConnParams(connInfo.getConnHandle(), 12, 12, 2, 100);
        deviceConnected = true;
    }

    // NimBLE 2.x Signature: onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason)
    void onDisconnect(BLEServer *pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        downloadFlag = false;
        INFO("*** App disconnected (reason: %d)", reason);
    }
};

class otaCallback : public BLECharacteristicCallbacks {
    void cleanUp() {
        downloadFlag = false;
        ota_size = 0;
        total_received = 0;
    }

    // NimBLE 2.x Signature: onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo)
    void onWrite(BLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string rxData = pCharacteristic->getValue();

        if (!downloadFlag && ota_size == 0) {
            INFO("Waiting for OTA size: %s", rxData.c_str());
            if (rxData.rfind("OTA_SIZE:", 0) == 0) {
                std::string sizeStr = rxData.substr(9);
                ota_size = std::stoul(sizeStr);
                INFO("OTA size: %d", ota_size);
            }
            return;
        }

        bufferCount++;
        total_received += rxData.length() * chunkCount;
        // uint8_t firstByte = rxData[0];
        // INFO("Progress R: %d T: %d C: %d B: %x", total_received, ota_size, rxData.length(), firstByte);

        if (!downloadFlag && ota_size > 0) {
            update_partition = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "app");
            
            // Fallback logic handled by esp_ota_get_next_update_partition normally, 
            // but we are forcing specific behavior here per original code.
            if(update_partition == NULL) {
                 INFO("Partition OTA_0 not found, trying automatic next");
                 update_partition = esp_ota_get_next_update_partition(NULL);
            }
            assert(update_partition != NULL);

            INFO("Writing to partition subtype %d at offset 0x%x",
                          update_partition->subtype, (unsigned int)update_partition->address);

            // WDT setup
            esp_task_wdt_config_t twdt_config = {
                .timeout_ms = 10000,
                .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
                .trigger_panic = false
            };
            esp_task_wdt_init(&twdt_config);
            esp_task_wdt_add(NULL); 

            if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &otaHandler) != ESP_OK) {
                cleanUp();
                return;
            }
            downloadFlag = true;
        }

        if (bufferCount >= 1 || rxData.length() > 0) {
            esp_task_wdt_reset(); // Feed dog
            if (esp_ota_write(otaHandler, (uint8_t *)rxData.c_str(), rxData.length()) != ESP_OK) {
                INFO("Error: write to flash failed");
                cleanUp();
                return;
            } else {
                bufferCount = 1;
                pTxCharacteristic->setValue(&txValue, 1);
                pTxCharacteristic->notify();
            }

            if (ota_size == total_received) {
                INFO("Final byte arrived");
                if (esp_ota_end(otaHandler) != ESP_OK) {
                    INFO("OTA end failed ");
                    cleanUp();
                    return;
                }

                INFO("Set Boot partition");
                if (ESP_OK == esp_ota_set_boot_partition(update_partition)) {
                    cleanUp();
                    
                    // Reset reboot counter in Meshtastic namespace
                    nvs_handle_t mesh_nvs;
                    if (nvs_open("meshtastic", NVS_READWRITE, &mesh_nvs) == ESP_OK) {
                        nvs_set_u32(mesh_nvs, "rebootCounter", 0);
                        nvs_commit(mesh_nvs);
                        nvs_close(mesh_nvs);
                    }

                    INFO("Restarting...");
                    vTaskDelay(2000 / portTICK_PERIOD_MS);
                    esp_restart();
                    return;
                } else {
                    INFO("Upload Error");
                    cleanUp();
                    return;
                }
            }
        } else {
            cleanUp();
        }
    }
};

// --- WiFi Logic ---
static const int wifi_connect_retries = 10;
static EventGroupHandle_t event_group_handle;
const int BIT_CONNECTED = BIT0;
const int BIT_FAIL = BIT1;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
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

static void wifi_connect(const wifi_credentials_t *config) {
    event_group_handle = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
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
    if (bits & BIT_FAIL) {
        FAIL("Failed to connect to WiFi");
    }
}

// --- Net OTA Logic (espota.py compatible) ---
typedef enum {
    ESPOTA_CMD_FLASH  =   0,
    ESPOTA_CMD_SPIFFS = 100,
} espota_cmd_t;

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
    
    // Ensure we parse exactly what is expected. 
    // buffer must be null-terminated by the caller.
    int res = sscanf(buffer, "%d %u %u %32s", &command, &remote_port, &firmware_size, md5);
    
    if (res != 4) FAIL("Invalid header format");
    
    config->cmd = (espota_cmd_t)command;
    config->remote_port = htons(remote_port);
    config->firmware_size = firmware_size;
    
    uint8_t md5_bytes[16];
    if (!md5_string_to_bytes(md5, md5_bytes)) FAIL("Invalid MD5 string");
    memcpy(config->firmware_md5, md5_bytes, sizeof(md5_bytes));
}

// ---------------------------------------------------------
// OTA Flash Logic (Corrected for IDF 5.x / MbedTLS 3.x)
// ---------------------------------------------------------
static void ota_flash() {
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

// --- Main App ---

std::string getDeviceName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[20];
    snprintf(name, sizeof(name), "Meshtastic_%02x%02x", mac[4], mac[5]);
    return std::string(name);
}

extern "C" void app_main(void) {
    // Initialize NVS
    nvs_init_custom("ota-wifi");

    INFO("\n\n//\\ E S H T /\\ S T / C Failsafe OTA\n\n");
    
    wifi_credentials_t config;
    nvs_read_config(&config);

    if (config.method == 1) {
        INFO("Mode: WiFi OTA");
        INFO("Connecting to SSID: %s", config.ssid);
        wifi_connect(&config);
        ota_flash();
        INFO("Marking NVRAM as updated.");
        nvs_mark_updated();
        INFO("Success. Rebooting.");
        esp_restart();
    } else {
        INFO("Mode: BLE OTA");
        
        // Read Meshtastic NVS info
        nvs_handle_t mesh_handle;
        uint32_t rebootCounter = 0;
        uint8_t hwven = 0;
        char fwrev[32] = {0};
        size_t fwrev_len = sizeof(fwrev);

        // Try to read meshtastic info, default if fail
        if (nvs_open("meshtastic", NVS_READONLY, &mesh_handle) == ESP_OK) {
            nvs_get_u32(mesh_handle, "rebootCounter", &rebootCounter);
            nvs_get_u8(mesh_handle, "hwVendor", &hwven);
            nvs_get_str(mesh_handle, "firmwareVersion", fwrev, &fwrev_len);
            nvs_close(mesh_handle);
        }

        INFO("Reboots: %d, HW: %d, FW: %s", (int)rebootCounter, hwven, fwrev);

        // Ensure we boot back to app next time (if manual reset)
        const esp_partition_t* next_part = esp_ota_get_next_update_partition(NULL);
        if (next_part) esp_ota_set_boot_partition(next_part);

        std::string manData = std::to_string(hwven) + "|" + std::string(fwrev);

        // Init NimBLE
        NimBLEDevice::init(getDeviceName());
        NimBLEDevice::setMTU(517);
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);

        pServer = NimBLEDevice::createServer();
        pServer->setCallbacks(new MyServerCallbacks());

        NimBLEService *pService = pServer->createService(SERVICE_UUID);
        pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
        pOtaCharacteristic = pService->createCharacteristic(CHARACTERISTIC_OTA_UUID, NIMBLE_PROPERTY::WRITE_NR);
        pOtaCharacteristic->setCallbacks(new otaCallback());

        pService->start();

        NimBLEAdvertising *pAdvertising = pServer->getAdvertising();
        pAdvertising->addServiceUUID(pService->getUUID());
        pAdvertising->setManufacturerData(manData);
        pAdvertising->start();

        INFO("Advertising started. Waiting for client.");

        // Loop replacement
        while(1) {
            if (!deviceConnected && oldDeviceConnected) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
                pServer->startAdvertising();
                INFO("Restart advertising");
                oldDeviceConnected = deviceConnected;
            }
            if (deviceConnected && !oldDeviceConnected) {
                INFO("Client connected");
                oldDeviceConnected = deviceConnected;
            }
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

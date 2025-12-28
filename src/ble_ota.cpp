#include "ble_ota.h"
#include "common_log.h"
#include "ota_processor.h" 
#include "nvs_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "NimBLEDevice.h"
#include <string>

#define TAG "BLE_OTA"
#define SERVICE_UUID "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define CHARACTERISTIC_TX_UUID "62ec0272-3ec5-11eb-b378-0242ac130003"
#define CHARACTERISTIC_OTA_UUID "62ec0272-3ec5-11eb-b378-0242ac130005"

// Buffer size: Enough to hold a few MTU packets. 
// MTU ~517. 4KB buffer provides enough slack for flash write latency.
#define STREAM_BUFFER_SIZE 4096 

static BLEServer *pServer = NULL;
static BLECharacteristic *pTxCharacteristic;
static BLECharacteristic *pOtaCharacteristic;
static bool deviceConnected = false;
static bool oldDeviceConnected = false;
static OtaProcessor otaProcessor;
static StreamBufferHandle_t xStreamBuffer = NULL;

static std::string getDeviceName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[20];
    snprintf(name, sizeof(name), "Meshtastic_%02x%02x", mac[4], mac[5]);
    return std::string(name);
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer, NimBLEConnInfo& connInfo) override {
        // Boost power and request faster intervals
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL1, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

        pServer->updateConnParams(connInfo.getConnHandle(), 12, 12, 0, 400); 
        
        deviceConnected = true;
        
        // Reset Processor and Buffer
        otaProcessor.reset();
        otaProcessor.setAckEnabled(true);
        xStreamBufferReset(xStreamBuffer); 
        
        otaProcessor.setSender([](const char* data, size_t len) {
            pTxCharacteristic->setValue((const uint8_t*)data, len);
            pTxCharacteristic->notify();
        });
        
        INFO("BLE Client Connected");
    }

    void onDisconnect(BLEServer *pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        otaProcessor.reset();
        INFO("App disconnected (reason: %d)", reason);
    }
};

class otaCallback : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
        std::string rxData = pCharacteristic->getValue();
        if (rxData.length() > 0) {
            // Non-blocking push to buffer. 
            // If buffer is full, we drop data (wait time 0).
            // Flow control relies on the Client not sending too fast.
            size_t bytesSent = xStreamBufferSend(xStreamBuffer, rxData.data(), rxData.length(), 0);
            
            if (bytesSent != rxData.length()) {
                // This log indicates the Swift delay is too short
                // Using printf directly as this runs in BLE context, avoid heavy logging if possible
                printf("E: StreamBuffer Full! Dropped %u bytes\n", (unsigned int)(rxData.length() - bytesSent));
            }
        }
    }
};

void ble_ota_task(void *param) {
    // Create Stream Buffer
    xStreamBuffer = xStreamBufferCreate(STREAM_BUFFER_SIZE, 1);
    if (xStreamBuffer == NULL) {
        FAIL("Failed to create StreamBuffer");
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);

    uint32_t rebootCounter = 0; 
    uint8_t hwven = 0; 
    char fwrev[32] = {0};
    size_t fwrev_len = sizeof(fwrev);
    nvs_get_meshtastic_info(&rebootCounter, &hwven, fwrev, fwrev_len);
    INFO("BLE Init - FW: %s", fwrev);

    std::string manData = std::to_string(hwven) + "|" + std::string(fwrev);
    
    NimBLEDevice::init(getDeviceName());
    NimBLEDevice::setMTU(517); 
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); 
    
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    NimBLEService *pService = pServer->createService(SERVICE_UUID);
    pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    pOtaCharacteristic = pService->createCharacteristic(CHARACTERISTIC_OTA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pOtaCharacteristic->setCallbacks(new otaCallback());
    
    pService->start();
    
    NimBLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->addServiceUUID(pService->getUUID());
    pAdvertising->setManufacturerData(manData);
    pAdvertising->start();
    
    INFO("BLE Advertising started.");

    uint8_t rxBuffer[520]; // Temp buffer to read from StreamBuffer

    while(1) {
        // 1. Process Incoming Data
        // Wait up to 10ms for data. If data arrives, process it immediately.
        // This effectively throttles the loop to the incoming data rate or idle rate.
        size_t receivedBytes = xStreamBufferReceive(xStreamBuffer, rxBuffer, sizeof(rxBuffer), 10 / portTICK_PERIOD_MS);
        
        if (receivedBytes > 0) {
            otaProcessor.process(rxBuffer, receivedBytes);
        }

        // 2. Connection Maintenance
        if (!deviceConnected && oldDeviceConnected) {
            // Just disconnected
            vTaskDelay(500 / portTICK_PERIOD_MS); 
            pServer->startAdvertising();
            INFO("Restart advertising");
            oldDeviceConnected = deviceConnected;
        }
        if (deviceConnected && !oldDeviceConnected) {
            oldDeviceConnected = deviceConnected;
        }

        // 3. Check for Reboot Flag
        if (otaProcessor.isRebootRequired()) {
            INFO("Reboot flag detected. Restarting in 2 seconds...");
            vTaskDelay(2000 / portTICK_PERIOD_MS); // Allow time for BLE notification to flush
            esp_restart();
        }

        esp_task_wdt_reset();
    }
}

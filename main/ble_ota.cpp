#include "ble_ota.h"
#include "common_log.h"
#include "nvs_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"
#include "esp_mac.h"
#include "NimBLEDevice.h"
#include <string>

#define TAG "BLE_OTA"
#define SERVICE_UUID "4FAFC201-1FB5-459E-8FCC-C5C9C331914B"
#define CHARACTERISTIC_TX_UUID "62ec0272-3ec5-11eb-b378-0242ac130003"
#define CHARACTERISTIC_OTA_UUID "62ec0272-3ec5-11eb-b378-0242ac130005"

static BLEServer *pServer = NULL;
static BLECharacteristic *pTxCharacteristic;
static BLECharacteristic *pOtaCharacteristic;
static bool deviceConnected = false;
static bool oldDeviceConnected = false;
static esp_ota_handle_t otaHandler = 0;
static const esp_partition_t *update_partition = NULL;
static uint8_t txValue = 0;
static int bufferCount = 0;
static size_t ota_size = 0;
static size_t total_received = 0;
static bool downloadFlag = false;

static std::string getDeviceName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[20];
    snprintf(name, sizeof(name), "Meshtastic_%02x%02x", mac[4], mac[5]);
    return std::string(name);
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer, NimBLEConnInfo& connInfo) override {
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL1, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

        //INFO(NimBLEAddress(desc->peer_ota_addr).toString().c_str());
        /*    We can use the connection handle here to ask for different connection
       parameters. Args: connection handle, min connection interval, max
       connection interval latency, supervision timeout. Units; Min/Max
       Intervals: 1.25 millisecond increments. Latency: number of intervals
       allowed to skip. Timeout: 10 millisecond increments, try for 5x interval
       time for best results.
        */
        pServer->updateConnParams(connInfo.getConnHandle(), 12, 12, 2, 100);
        deviceConnected = true;
        deviceConnected = true;
    }
    void onDisconnect(BLEServer *pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        downloadFlag = false;
        INFO("*** App disconnected (reason: %d)", reason);
    }
};

class otaCallback : public BLECharacteristicCallbacks {
    uint8_t chunkCount = 1;

    void cleanUp() {
        if(otaHandler) esp_ota_end(otaHandler);
        downloadFlag = false;
        ota_size = 0;
        total_received = 0;
        otaHandler = 0;
    }

  void onWrite(BLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string rxData = pCharacteristic->getValue();

    if (!downloadFlag && ota_size == 0) {
      // Check if the string starts with "OTA_SIZE:"
      INFO("Waiting for OTA size: %s\n\r", rxData.c_str());
      if (rxData.rfind("OTA_SIZE:", 0) == 0) {
        // Strip away "OTA_SIZE:"
        std::string sizeStr = rxData.substr(9);

        // Convert the remaining string to an integer
        ota_size = std::stoul(sizeStr);
        INFO("OTA size: %d\n\r", ota_size);
      }
      return;
    }

    bufferCount++;
    total_received += rxData.length() * chunkCount;
    u_int8_t firstByte = rxData[0]; // print first byte as hex
    INFO("Progress R: %d T: %d C: %d B: %x\n\r", total_received,
                  ota_size, rxData.length(), firstByte);

    if (!downloadFlag && ota_size > 0) {
      // First BLE bytes have arrived

      update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (update_partition == NULL) {
        FAIL("PARTITION IS NULL") ;
      }

      INFO("Writing to partition subtype %d at offset 0x%lx \n\r",
                    update_partition->subtype, update_partition->address);

      // esp_ota_begin can take a while to complete as it erase the flash
      // partition (3-5 seconds) so make sure there's no timeout on the client
      // side that triggers before that.
      esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 10000,
        .idle_core_mask = (1 << 0), // Watch Core 0
        .trigger_panic = false
      };
      esp_task_wdt_init(&wdt_config);
      esp_task_wdt_add(NULL); // Add current task to WDT

      if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &otaHandler) !=
          ESP_OK) {
        cleanUp();
        return;
      }
      downloadFlag = true;
    }

    if (bufferCount >= 1 || rxData.length() > 0) {
      if (esp_ota_write(otaHandler, (uint8_t *)rxData.c_str(),
                        rxData.length()) != ESP_OK) {
        INFO("Error: write to flash failed");
        cleanUp();
        return;
      } else {
        bufferCount = 1;
        // Notify the app so next batch can be sent
        pTxCharacteristic->setValue(&txValue, 1);
        pTxCharacteristic->notify();
      }

      // check if this was the last data chunk? If so, finish the OTA update
      if (ota_size == total_received) {
        INFO("Final byte arrived");
        // Final chunk arrived. Now check that
        // the length of total file is correct
        if (esp_ota_end(otaHandler) != ESP_OK) {
          INFO("OTA end failed ");
          cleanUp();
          return;
        }

        // Clear download flag and restart the ESP32 if the firmware
        // update was successful
        INFO("Set Boot partion");
        if (ESP_OK == esp_ota_set_boot_partition(update_partition)) {
          esp_ota_end(otaHandler);
          cleanUp();
          nvs_reset_meshtastic_counter();
          INFO("Restarting...");
          vTaskDelay(2000 / portTICK_PERIOD_MS);
          esp_restart();
          return;
        } else {
          // Something whent wrong, the upload was not successful
          INFO("Upload Error");
          cleanUp();
          esp_ota_end(otaHandler);
          return;
        }
      }
    } else {
      cleanUp();
    }
  }
};

void ble_ota_task(void *param) {
    vTaskDelay(500 / portTICK_PERIOD_MS);

    uint32_t rebootCounter = 0; 
    uint8_t hwven = 0; 
    char fwrev[32] = {0};
    nvs_get_meshtastic_info(&rebootCounter, &hwven, fwrev, sizeof(fwrev));
    INFO("Reboots: %d, HW: %d, FW: %s", (int)rebootCounter, hwven, fwrev);

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
    
    INFO("BLE Advertising started. Waiting for client.");

    while(1) {
        if (!deviceConnected && oldDeviceConnected) {
            vTaskDelay(500 / portTICK_PERIOD_MS); 
            pServer->startAdvertising();
            INFO("Restart advertising");
            oldDeviceConnected = deviceConnected;
        }
        if (deviceConnected && !oldDeviceConnected) {
            INFO("Client connected");
            oldDeviceConnected = deviceConnected;
        }
        // Feed watchdog in the loop
        esp_task_wdt_reset();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

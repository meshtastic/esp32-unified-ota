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
#include "mbedtls/sha256.h" // Added for SHA-256 hash calculation
#include <string>
#include <cstring> // Added for memcmp
#include "utils.h"

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
static size_t ota_size = 0;
static size_t total_received = 0;
static bool downloadFlag = false;
static uint8_t firmware_hash[32]; // Added: To store the expected SHA-256 hash
static mbedtls_sha256_context sha256_ctx; // Added: SHA-256 context

// Copied from net_ota.cpp for consistent header parsing logic
typedef enum { ESPOTA_CMD_FLASH = 0, ESPOTA_CMD_SPIFFS = 100 } espota_cmd_t;

// Copied from net_ota.cpp for hash string conversion
static bool hash_string_to_bytes(const char *hex, uint8_t *bytes) {
    for (int i = 0; i < 32; ++i) {  // 32 bytes for SHA-256
        unsigned int byte = 0;
        if (sscanf(&hex[i * 2], "%02x", &byte) != 1) return false;
        bytes[i] = (uint8_t)byte;
    }
    return true;
}

static std::string getDeviceName() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char name[20];
    snprintf(name, sizeof(name), "Meshtastic_%02x%02x", mac[4], mac[5]);
    return std::string(name);
}

void notifyStatus(bleota_status_t newState) {
    // Cast the enum class value to its underlying type (uint8_t)
    uint8_t statusByte = static_cast<uint8_t>(newState);
    pTxCharacteristic->setValue(&statusByte, 1); // Pass address of uint8_t and size 1
    pTxCharacteristic->notify();
}

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer, NimBLEConnInfo& connInfo) override {
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL1, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

        pServer->updateConnParams(connInfo.getConnHandle(), 12, 12, 2, 100);
        deviceConnected = true;

        notifyStatus(WAITING_FOR_SIZE);
    }
    void onDisconnect(BLEServer *pServer, NimBLEConnInfo& connInfo, int reason) override {
        deviceConnected = false;
        downloadFlag = false;
        INFO("*** App disconnected (reason: %d)", reason);
    }
};

class otaCallback : public BLECharacteristicCallbacks {
    void cleanUp(bool hash_failed = false) {
        if(otaHandler) {
            esp_ota_end(otaHandler);
            otaHandler = 0; // Reset handle after ending
        }
        // If hash failed, and we have an update_partition, explicitly corrupt its header
        // This is the explicit "leave in non-executable state" part.
        if (hash_failed && update_partition) {
            INFO("Hash check failed: Corrupting header of target partition at 0x%lx", update_partition->address);
            corrupt_partition(update_partition);
        }

        mbedtls_sha256_free(&sha256_ctx); // Free SHA-256 context regardless of success/failure
        downloadFlag = false;
        ota_size = 0;
        total_received = 0;
    }

  void onWrite(BLECharacteristic *pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string rxData = pCharacteristic->getValue();

    if (rxData.empty()) {
        INFO("Received empty data, ignoring.");
        return;
    }

    if (!downloadFlag && ota_size == 0) {
      // Expecting "command port filesize hash"
      int command = 0;
      unsigned int remote_port = 0; // ignored for BLE
      unsigned int received_ota_size = 0;
      char hash_hex[65] = { 0 }; // 64 hex chars + null terminator

      // Parse the incoming header string
      int res = sscanf(rxData.c_str(), "%d %u %u %64s", &command, &remote_port, &received_ota_size, hash_hex);

      if (res != 4 || command != ESPOTA_CMD_FLASH) { // Assuming ESPOTA_CMD_FLASH for BLE OTA
        INFO("Invalid OTA header format or command: %s", rxData.c_str());
        notifyStatus(ERROR);
        cleanUp(); // Reset state
        return;
      }
      
      ota_size = received_ota_size;
      INFO("OTA command: %d, Port: %u (ignored), Size: %u, Hash: %s", command, remote_port, (unsigned int)ota_size, hash_hex);

      // Convert hash string to bytes
      if (!hash_string_to_bytes(hash_hex, firmware_hash)) {
          INFO("Invalid SHA-256 hash string in header.");
          notifyStatus(ERROR);
          cleanUp(); // Reset state
          return;
      }

      // Initialize SHA-256 context for the new download
      mbedtls_sha256_init(&sha256_ctx);
      if (mbedtls_sha256_starts(&sha256_ctx, 0) != 0) { // 0 for SHA256
          INFO("SHA-256 start failed");
          notifyStatus(ERROR);
          cleanUp();
          return;
      }

      notifyStatus(ERASING_FLASH);

      // We are targeting APP_OTA_0. For this to work, the running application
      // must be on a different partition (e.g., factory or APP_OTA_1).
      // If your partitions.csv has only one app partition, this OTA will fail
      // or corrupt the running firmware.
      update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
      if (update_partition == NULL) {
        // If you don't want a restart here, change FAIL to notifyStatus and return.
        FAIL("Could not find update partition."); 
      }

      INFO("Destination partition is subtype %d at offset 0x%lx \n\r",
                  update_partition->subtype, update_partition->address);

      // Safety: Set boot partition to current before starting, in case of power loss
      // This is crucial for your "no flip-flop" approach combined with explicit bad-marking.
      // If OTA fails at any point before esp_ota_set_boot_partition(update_partition)
      // is successfully called, the system will revert to this running partition.
      const esp_partition_t *running = esp_ota_get_running_partition();
      esp_ota_set_boot_partition(running);

      if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &otaHandler) != ESP_OK) {
        INFO("esp_ota_begin failed");
        cleanUp();
        notifyStatus(ERROR);
        return;
      }

      notifyStatus(READY_FOR_CHUNK);
      downloadFlag = true;
      return;
    }
    
    // Update SHA-256 hash with the received data chunk
    if (mbedtls_sha256_update(&sha256_ctx, (const unsigned char*)rxData.c_str(), rxData.length()) != 0) {
        INFO("SHA-256 update failed");
        cleanUp();
        notifyStatus(ERROR);
        return;
    }

    // This part handles actual data chunks
    total_received += rxData.length();
    uint8_t firstByte = rxData[0]; 
    INFO("Progress R: %u T: %u L: %u B: %02x\n\r", total_received,
                  (unsigned int)ota_size, (unsigned int)rxData.length(), firstByte);

    if (esp_ota_write(otaHandler, (uint8_t *)rxData.c_str(), rxData.length()) != ESP_OK) {
        INFO("Error: write to flash failed");
        notifyStatus(ERROR);
        cleanUp();
        return;
    } else {
        // Notify the app so next batch can be sent
        notifyStatus(CHUNK_ACK);
    }

    // check if this was the last data chunk? If so, finish the OTA update
    if (ota_size == total_received) {
      INFO("Final byte arrived");
      // Final chunk arrived. Now check that
      // the length of total file is correct
      if (esp_ota_end(otaHandler) != ESP_OK) {
        INFO("OTA end failed ");
        notifyStatus(ERROR);
        cleanUp();
        return;
      }

      // Finalize SHA-256 hash and compare
      uint8_t calculated_digest[32];
      if (mbedtls_sha256_finish(&sha256_ctx, calculated_digest) != 0) {
          INFO("SHA-256 finish failed");
          cleanUp();
          notifyStatus(ERROR);
          return;
      }
      // mbedtls_sha256_free is now called in cleanUp()

      if (memcmp(calculated_digest, firmware_hash, 32) != 0) {
          printf("Calculated SHA-256: "); for(int i=0; i<32; i++) printf("%02x", calculated_digest[i]); printf("\n");
          printf("Expected SHA-256   : "); for(int i=0; i<32; i++) printf("%02x", firmware_hash[i]); printf("\n");
          INFO("Error: SHA-256 Mismatch");
          notifyStatus(ERROR);
          cleanUp(true); // Pass true to indicate hash failure for header corruption
          return;
      }
      INFO("SHA-256 hash verified successfully.");

      // Clear download flag and restart the ESP32 if the firmware
      // update was successful
      INFO("Set Boot partion");
      if (ESP_OK == esp_ota_set_boot_partition(update_partition)) {
        notifyStatus(OTA_COMPLETE);
        cleanUp(); // No hash failure, just general cleanup
        nvs_reset_meshtastic_counter();
        INFO("Restarting...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
      } else {
        // Something went wrong, the upload was not successful
        INFO("Upload Error: Failed to set boot partition.");
        notifyStatus(ERROR);
        cleanUp(); // No hash failure, but general OTA error
        return;
      }
    }
  }
};

void ble_ota_task(void *param) {
    vTaskDelay(500 / portTICK_PERIOD_MS);

    uint32_t rebootCounter = 0; 
    uint8_t hwven = 0; 
    char fwrev[32] = {0};
    size_t fwrev_len = sizeof(fwrev);
    nvs_get_meshtastic_info(&rebootCounter, &hwven, fwrev, fwrev_len);
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

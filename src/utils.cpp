#include "common_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <cstring>
#include <cstdio>

#define TAG "UTILS"


void corrupt_partition(const esp_partition_t *partition) {
   uint8_t dummy_data[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // Overwrite the magic byte 0xE9
   // A small write to offset 0 will make the partition unbootable by the ESP32 bootloader
   if (esp_partition_write(partition, 0, dummy_data, sizeof(dummy_data)) != ESP_OK) {
       INFO("Failed to corrupt partition header!");
   } else {
       INFO("Partition header corrupted successfully.");
   }
}
#pragma once

typedef enum { WAITING_FOR_SIZE = 0, ERASING_FLASH = 1, READY_FOR_CHUNK = 2, CHUNK_ACK = 3, OTA_COMPLETE = 4, ERROR = 5  } bleota_status_t;

void ble_ota_task(void *param);

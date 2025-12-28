#include "ota_processor.h"
#include "common_log.h"
#include "nvs_config.h"
#include "utils.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

#define TAG "OTA_PROC"

#define RESP_OK "OK\n"
#define RESP_ERR "ERR\n"
#define RESP_ACK "ACK" // No newline needed for BLE packets usually, but keeps it simple

OtaProcessor::OtaProcessor() : _state(STATE_IDLE), _sender(nullptr), _reboot_required(false), _ack_enabled(false) {
    reset();
}

OtaProcessor::~OtaProcessor() {
    cleanup(false);
}

void OtaProcessor::setSender(ota_sender_t sender) {
    _sender = sender;
}

// NEW: Setter for ACK mode
void OtaProcessor::setAckEnabled(bool enabled) {
    _ack_enabled = enabled;
}

void OtaProcessor::reset() {
    cleanup(false);
    _state = STATE_IDLE;
    _reboot_required = false;
    // Do not reset _ack_enabled here, it's a configuration setting
    _cmd_len = 0;
    memset(_cmd_buffer, 0, sizeof(_cmd_buffer));
}

bool OtaProcessor::isRebootRequired() const {
    return _reboot_required;
}

void OtaProcessor::cleanup(bool success) {
    if (_ota_handle) {
        if (!success) esp_ota_abort(_ota_handle);
        _ota_handle = 0;
    }
    mbedtls_sha256_free(&_sha_ctx);
    _firmware_size = 0;
    _total_received = 0;
}

void OtaProcessor::sendResponse(const char* fmt, ...) {
    if (!_sender) return;
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    _sender(buf, strlen(buf));
}

void OtaProcessor::process(const uint8_t* data, size_t len) {
    if (len == 0) return;

    if (_state == STATE_DOWNLOADING) {
        handleBinaryChunk(data, len);
    } else {
        for (size_t i = 0; i < len; i++) {
            if (_cmd_len < sizeof(_cmd_buffer) - 1) {
                char c = (char)data[i];
                if (c == '\n' || c == '\r') {
                    if (_cmd_len > 0) {
                        _cmd_buffer[_cmd_len] = 0;
                        handleCommand();
                        _cmd_len = 0;
                    }
                } else {
                    _cmd_buffer[_cmd_len++] = c;
                }
            } else {
                INFO("Command buffer overflow");
                _cmd_len = 0;
            }
        }
    }
}

void OtaProcessor::handleCommand() {
    INFO("CMD: %s", _cmd_buffer);
    if (strncmp(_cmd_buffer, "VERSION", 7) == 0) {
        handleVersion();
    } else if (strncmp(_cmd_buffer, "REBOOT", 6) == 0) {
        handleReboot();
    } else if (strncmp(_cmd_buffer, "OTA", 3) == 0) {
        handleOtaStart(_cmd_buffer + 3);
    } else {
        sendResponse("ERR Unknown Command\n");
    }
}

void OtaProcessor::handleVersion() {
    uint32_t reboot_counter = 0;
    uint8_t hw_vendor = 0;
    char fw_rev[32] = {0};
    size_t fw_rev_len = sizeof(fw_rev);
    nvs_get_meshtastic_info(&reboot_counter, &hw_vendor, fw_rev, fw_rev_len);
    // Append version + git hash
    sendResponse("OK %d %s %lu v%s\n", hw_vendor, fw_rev, (unsigned long)reboot_counter, GIT_VERSION);
}

void OtaProcessor::handleReboot() {
    sendResponse(RESP_OK);
    INFO("Reboot command received");
    _reboot_required = true;
}

static bool hash_string_to_bytes(const char *hex, uint8_t *bytes) {
    for (int i = 0; i < 32; ++i) {
        unsigned int byte = 0;
        if (sscanf(&hex[i * 2], "%02x", &byte) != 1) return false;
        bytes[i] = (uint8_t)byte;
    }
    return true;
}

void OtaProcessor::handleOtaStart(const char* args) {
    unsigned int size = 0;
    char hash_hex[65] = {0};

    if (sscanf(args, "%u %64s", &size, hash_hex) != 2) {
        sendResponse("ERR Invalid Format\n");
        return;
    }

    if (!hash_string_to_bytes(hash_hex, _expected_hash)) {
        sendResponse("ERR Invalid Hash\n");
        return;
    }

    _firmware_size = size;
    _target_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    
    if (!_target_partition) {
        sendResponse("ERR No Partition\n");
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_set_boot_partition(running); 

    INFO("Starting OTA. Size: %u, Part: 0x%lx", (unsigned int)_firmware_size, _target_partition->address);

    if (esp_ota_begin(_target_partition, _firmware_size, &_ota_handle) != ESP_OK) {
        sendResponse("ERR OTA Begin Failed\n");
        return;
    }

    mbedtls_sha256_init(&_sha_ctx);
    mbedtls_sha256_starts(&_sha_ctx, 0); 

    _state = STATE_DOWNLOADING;
    _total_received = 0;
    sendResponse(RESP_OK);
}

void OtaProcessor::handleBinaryChunk(const uint8_t* data, size_t len) {
    if (mbedtls_sha256_update(&_sha_ctx, data, len) != 0) {
        INFO("Hash update failed");
        sendResponse("ERR Hash Update\n");
        reset();
        return;
    }

    if (esp_ota_write(_ota_handle, data, len) != ESP_OK) {
        INFO("Flash write failed");
        sendResponse("ERR Flash Write\n");
        reset();
        return;
    }

    _total_received += len;

    if (_total_received % 65536 == 0 || _total_received == _firmware_size) {
        INFO("Progress: %u / %u", (unsigned int)_total_received, (unsigned int)_firmware_size);
    }

    if (_total_received >= _firmware_size) {
        if (_total_received > _firmware_size) {
            INFO("Received too much data!");
            sendResponse("ERR Size Mismatch\n");
            reset();
        } else {
            endOta();
        }
    } else {
        // NEW: Send ACK if configured (for BLE)
        // Only send ACK if we aren't done yet. If we are done, endOta() sends OK.
        if (_ack_enabled) {
            sendResponse(RESP_ACK);
        }
    }
}

void OtaProcessor::endOta() {
    uint8_t calculated_hash[32];
    mbedtls_sha256_finish(&_sha_ctx, calculated_hash);
    
    if (memcmp(calculated_hash, _expected_hash, 32) != 0) {
        INFO("Hash Mismatch");
        print_hash("Calc: ", calculated_hash);
        print_hash("Exp : ", _expected_hash);
        sendResponse("ERR Hash Mismatch\n");
        esp_ota_abort(_ota_handle);
        _ota_handle = 0; 
        reset();
        return;
    }

    if (esp_ota_end(_ota_handle) != ESP_OK) {
        INFO("OTA End failed");
        sendResponse("ERR OTA End\n");
        reset();
        return;
    }
    _ota_handle = 0;

    if (esp_ota_set_boot_partition(_target_partition) != ESP_OK) {
        INFO("Set boot failed");
        sendResponse("ERR Set Boot\n");
        reset();
        return;
    }

    nvs_reset_meshtastic_counter();
    sendResponse(RESP_OK);
    INFO("OTA Success. Flagging reboot.");
    _reboot_required = true;
}

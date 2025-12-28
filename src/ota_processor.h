#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

// Callback type for sending responses (e.g. "OK\n", "ERR...")
typedef std::function<void(const char* data, size_t len)> ota_sender_t;

class OtaProcessor {
public:
    OtaProcessor();
    ~OtaProcessor();

    void setSender(ota_sender_t sender);
    void process(const uint8_t* data, size_t len);
    void reset();
    bool isRebootRequired() const;

    // NEW: Enable explicit ACKs for binary chunks (For BLE flow control)
    void setAckEnabled(bool enabled);

private:
    enum State {
        STATE_IDLE,
        STATE_DOWNLOADING
    };

    State _state;
    ota_sender_t _sender;
    bool _reboot_required;
    bool _ack_enabled; // New flag
    
    esp_ota_handle_t _ota_handle;
    const esp_partition_t* _target_partition;
    size_t _firmware_size;
    size_t _total_received;
    uint8_t _expected_hash[32];
    mbedtls_sha256_context _sha_ctx;

    char _cmd_buffer[256];
    size_t _cmd_len;

    void handleCommand();
    void handleVersion();
    void handleReboot();
    void handleOtaStart(const char* args);
    void handleBinaryChunk(const uint8_t* data, size_t len);
    void endOta();
    void cleanup(bool success);
    void sendResponse(const char* fmt, ...);
};

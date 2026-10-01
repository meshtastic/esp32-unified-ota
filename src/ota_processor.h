#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psa/crypto.h"

// Callback type for sending responses (e.g. "OK\n", "ERR...")
typedef std::function<void(const char* data, size_t len)> ota_sender_t;

class OtaProcessor {
public:
    OtaProcessor();
    ~OtaProcessor();

    void setSender(ota_sender_t sender);
    void setNvramExpectedHash(const uint8_t* hash);

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
    bool _ack_enabled;
    
    uint8_t _nvs_expected_hash[32];
    bool _has_nvs_hash;

    esp_ota_handle_t _ota_handle;
    const esp_partition_t* _target_partition;
    size_t _firmware_size;
    size_t _total_received;
    uint8_t _expected_hash[32];
    psa_hash_operation_t _sha_op;

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

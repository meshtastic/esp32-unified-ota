# ESP32 Unified Updater (BLE and WiFi)

[![MemBrowse](https://membrowse.com/badge.svg)](https://membrowse.com/public/meshtastic/esp32-unified-ota)

The is an AI-fueled fever dream mash-up of the the WiFi and BLE OTA updaters for ESP-32

- BLE Updater: https://github.com/meshtastic/firmware-ota
- WiFi Updater: https://github.com/meshtastic/firmware-ota-wifi

## Unified OTA Protocol Specification

### 1. Overview

  The protocol uses a text-based command-response handshake followed by a raw binary stream. It is designed for constrained devices, minimizing RAM usage by streaming data directly to flash memory.

  Key Security Feature: The device strictly enforces firmware integrity. The SHA-256 hash of the intended firmware must be pre-provisioned in the device's Non-Volatile Storage (NVS) before the update begins. The client must provide this exact hash during the handshake, and the final downloaded binary must match it.

  Meshtastic's design implicitly trusts the network.  This NVS hashing system is intended for future compatibility.  Should the firmware TCP connection be secured in some way, then the NVS hash can only be applied by a trusted device.  The OTA loader will then only accept a firmware with the proper hash.  

  If there is a hash mismatch, the OTA loader will intentionally corrupt the header of the partition to prevent untrusted or bad code from executing.

### 2. Transport Layers

#### A. WiFi (TCP)
*   **Discovery:** The device listens for UDP broadcasts on port `3232`. It responds with its device name and version.
*   **Connection:** The Client initiates a TCP connection to the device IP on port `3232`.
*   **Flow:** Synchronous. The client sends a packet and waits for a response (or TCP ACK).

#### B. Bluetooth Low Energy (BLE)
*   **Service UUID:** `4FAFC201-1FB5-459E-8FCC-C5C9C331914B`
*   **Characteristics:**
    *   **OTA (Write / Write No Response):** `62ec0272-3ec5-11eb-b378-0242ac130005`
        *   Used by Client to send Commands and Binary Firmware.
    *   **TX (Notify):** `62ec0272-3ec5-11eb-b378-0242ac130003`
        *   Used by Device to send responses (`OK`, `ERR`, `ACK`, `ERASING`).
*   **Flow:** Asynchronous with application-level ACK. The client writes a chunk, then waits for an `ACK` notification before sending the next.

---

### 3. Protocol Workflow

#### Phase 1: Connection & Version Check
1.  **Client** connects (TCP) or Subscribes to TX Characteristic (BLE).
2.  **Client** sends: `VERSION\n`
3.  **Device** responds: `OK <hw_ver> <fw_ver> <reboot_count> <git_hash>\n`
    *   *Example:* `OK 1 1.0.0 45 v1.2-5-g8a2b3c`

#### Phase 2: The Handshake (Start OTA)
1.  **Client** sends: `OTA <size_in_bytes> <sha256_hex_string>\n`
    *   *Example:* `OTA 1048576 a1b2c3d4...` (64 char hex string)
2.  **Device** performs **Validation**:
    *   Parses size and hash.
    *   **Check:** Compares the Client-provided hash against the `ota_hash` stored in the device's NVS.
3.  **Device** responds (on success):
    *   Sends: `ERASING` (This indicates flash erasure has started. Client should wait).
    *   *...Time passes (Flash Erase)...*
    *   Sends: `OK\n` (Device is now ready to receive binary stream).
4.  **Device** responds (on failure):
    *   Sends: `ERR <Reason>\n` (e.g., `ERR Hash Rejected`, `ERR Invalid Format`).
    *   The connection remains open, but OTA state is reset.

#### Phase 3: Binary Streaming
Once the `OK\n` is received after the OTA command, the device enters `STATE_DOWNLOADING`.

1.  **Client** splits the firmware binary into chunks (Recommended: 256 to 512 bytes for BLE, up to 1024 for WiFi).
2.  **Loop** until all bytes are sent:
    *   **Client** sends binary chunk.
    *   **Device** writes chunk to OTA partition and updates running SHA-256 calculation.
    *   **(BLE ONLY) Device** sends: `ACK` via notification.
    *   **(BLE ONLY) Client** waits for `ACK` before sending next chunk.
    *   **(WiFi)** Standard TCP flow control applies; no application-level ACK is sent per chunk.

#### Phase 4: Finalization & Verification
1.  **Device** detects that `total_received_bytes == size_in_bytes`.
2.  **Device** calculates the final SHA-256 hash of the written partition.
3.  **Device** compares Calculated Hash vs. NVS Expected Hash.

##### Scenario A: Success
1.  **Device** sets the new partition as the Boot Partition.
2.  **Device** resets internal reboot counters (if applicable).
3.  **Device** sends: `OK\n`
4.  **Device** waits 2 seconds, then reboots.

##### Scenario B: Failure (Hash Mismatch)
1.  **Device** detects mismatch.
2.  **Device** executes **Partition Corruption**:
    *   Overwrites the first sector of the target OTA partition with garbage data (`0xFF`).
    *   This ensures the bootloader will **not** attempt to boot this corrupt firmware.
3.  **Device** sends: `ERR Hash Mismatch\n`
4.  **Device** aborts OTA and resets state.

---

### 4. Command Reference

All commands must be terminated with `\n` (newline).

| Command | Arguments | Description | Response Success | Response Fail |
| :--- | :--- | :--- | :--- | :--- |
| `VERSION` | None | Get device info | `OK <hw> <fw> <cnt> <ver>` | `ERR` |
| `REBOOT` | None | Force restart | `OK` (then reboots) | `ERR` |
| `OTA` | `<size> <hash>` | Start update | `ERASING` ... `OK` | `ERR <Msg>` |

### 5. Error Codes

If the device replies with `ERR`, the remainder of the line describes the error.

*   `ERR Unknown Command`: The command string was not recognized.
*   `ERR Invalid Format`: Arguments for `OTA` command were malformed.
*   `ERR Hash Rejected (NVS Mismatch)`: The hash provided by the client does not match the hash pinned in the device NVS.
*   `ERR No Partition`: Could not find a valid OTA_0 or OTA_1 partition.
*   `ERR OTA Begin Failed`: Hardware error initializing flash write.
*   `ERR Hash Update`: Internal crypto engine error.
*   `ERR Flash Write`: Hardware error writing to flash.
*   `ERR Size Mismatch`: Client sent more bytes than declared in `OTA` command.
*   `ERR Hash Mismatch`: The downloaded binary did not match the expected hash.
*   `ERR Set Boot`: Failed to configure bootloader to use new partition.

## Building with PlatformIO

Five environments produce one loader image per chip. CI and the release publish those five binaries (`mt-<chip>-ota.bin`). A bare `pio run` builds the same five.

| Env | Board | Transports | `pio upload` address |
| --- | --- | --- | --- |
| `esp32` | `esp32dev` | BLE + WiFi | `0x260000` |
| `esp32s2` | `esp32-s2-saola-1` | WiFi only | `0x260000` |
| `esp32s3` | `esp32-s3-devkitc-1` | BLE + WiFi | `0x650000` |
| `esp32c3` | `esp32-c3-devkitm-1` | BLE only | `0x260000` |
| `esp32c6` | `esp32-c6-devkitm-1` | BLE only | `0x260000` |

`pio run -e <chip> -t upload` writes that address and does not write a partition table. Other boards use the same `.pio/build/<chip>/firmware.bin` at the `ota_1` offset of the partition table the firmware was built with:

| Partition table | `ota_1` |
| --- | --- |
| `partition-table.csv` | `0x260000` |
| `partition-table-t3s3.csv` | `0x2A0000` |
| `default_8MB.csv` | `0x340000` |
| `partition-table-8MB.csv` | `0x5D0000` |
| `default_16MB.csv` | `0x650000` |

T3-S3 firmware that uses `partition-table-t3s3.csv` loads the loader at `0x2A0000`. A device still on the older 4 MB map has `ota_1` at `0x260000` until a full firmware flash.

```
pio run -e esp32s3
esptool.py write_flash 0x2A0000 .pio/build/esp32s3/firmware.bin
```

## Building with esp-idf

- Install esp-idf (v6.1 or later) and activate the environment

- Clean build with the following commands:

```
rm sdkconfig
rm -rf build
idf.py set-target esp32s3
idf.py build
```

- Install `build/OTA-WiFi.bin` at the `ota_1` offset in the table above. The image header is 4 MB. `0x260000` is `partition-table.csv`, not every 4 MB board. `0x5D0000` is `partition-table-8MB.csv`. `0x650000` is `default_16MB.csv`.


## Using this during development

- Apply merge the changes in the firmware `meshtastic-ota` branch into your firmware
  version of choice. 

- Build and install this version on your device

- Buind and install this OTA firmware into the OTA_1 partition.  Be careful only
  write the OTA_1 partition and not any of the supporting partitions.  The 
  platformio.ini file has a custom update step to do this using esptool.py

- Build and install the `firmware-updates` branch of the iOS app

- Good luck
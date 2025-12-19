# ESP32 Unified Updater (BLE and WiFi)

The is an AI-fueled fever dream mash-up of the the WiFi and BLE OTA updaters for ESP-32

- BLE Updater: https://github.com/meshtastic/firmware-ota
- WiFi Updater: https://github.com/meshtastic/firmware-ota-wifi

## Building

- Install esp-idf and activate the environment

- Clean build with the following commands:

```
rm sdkconfig
rm -rf build
idf.py set-target esp32s3
idf.py build
```

- Install it on a 4MB board:

  `esptool.py --port /dev/tty.usbmodem21101 write_flash 0x260000 ./build/OTA-WiFi.bin`

- Install it on a 8MB board:

  `esptool.py --port /dev/tty.usbmodem21101 write_flash 0x5D0000 ./build/OTA-WiFi.bin`
  
- Install it on a 16MB board:

  `esptool.py --port /dev/tty.usbmodem21101 write_flash 0x650000 ./build/OTA-WiFi.bin`

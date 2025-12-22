# ESP32 Unified Updater (BLE and WiFi)

The is an AI-fueled fever dream mash-up of the the WiFi and BLE OTA updaters for ESP-32

- BLE Updater: https://github.com/meshtastic/firmware-ota
- WiFi Updater: https://github.com/meshtastic/firmware-ota-wifi

## Building with PlatformIO

- PlatformIO should build with an esp32 or esp32s3 environment.

  ```
  pio run -e esp32s3 -t upload
  ```
  or
  ```
  pio run -e esp32 -t upload
  ```

- A custom `uplaod_command` in the ini file will upload only the OTA loader into the
  proper partion.

## Building with esp-idf

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


## Using this during development

- Apply merge the changes in the firmware `meshtastic-ota` branch into your firmware
  version of choice. 

- Build and install this version on your device

- Buind and install this OTA firmware into the OTA_1 partition.  Be careful only
  write the OTA_1 partition and not any of the supporting partitions.  The 
  platformio.ini file has a custom update step to do this using esptool.py

- Build and install the `firmware-updates` branch of the iOS app

- Good luck
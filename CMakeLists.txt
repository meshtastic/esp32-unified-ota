cmake_minimum_required(VERSION 3.16)

# Read the version file
file(STRINGS "version.txt" PROJECT_VER)

# ------------------------------------------------------------------------------
# 1. Determine Project Name & Validate Configuration
# ------------------------------------------------------------------------------
if(ENABLE_BLE_OTA AND ENABLE_WIFI_OTA)
    set(OTA_PROJECT_NAME "MeshtasticOTA")
elseif(ENABLE_BLE_OTA)
    set(OTA_PROJECT_NAME "MeshtasticOTA-BLE")
elseif(ENABLE_WIFI_OTA)
    set(OTA_PROJECT_NAME "MeshtasticOTA-WiFi")
else()
    # Condition 4: Both OFF -> Fail compilation
    message(FATAL_ERROR "Configuration Error: You must enable at least one OTA method (ENABLE_BLE_OTA=ON or ENABLE_WIFI_OTA=ON).")
endif()

# ------------------------------------------------------------------------------
# 2. Conditional Component Inclusion
# ------------------------------------------------------------------------------
# Only register the third_party directory (containing esp-nimble-cpp) 
# if BLE is enabled. This prevents build errors on targets like ESP32-S2.
if(ENABLE_BLE_OTA)
    get_filename_component(PROJECT_DIR ${CMAKE_CURRENT_LIST_DIR} ABSOLUTE)
    list(APPEND EXTRA_COMPONENT_DIRS "${PROJECT_DIR}/third_party")
endif()

# ------------------------------------------------------------------------------
# 3. Standard IDF Build Setup
# ------------------------------------------------------------------------------
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Size trims for mbedTLS/PSA Crypto that Kconfig can't express (see the header).
# esp_config.h includes it last; every component using mbedTLS/PSA headers must
# see the same config, so it is set build-wide.
idf_build_set_property(COMPILE_DEFINITIONS
    "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/src/mbedtls_user_config.h\""
    APPEND)

# Apply the dynamic name derived in Step 1
project(${OTA_PROJECT_NAME} VERSION "${PROJECT_VER}")

# ------------------------------------------------------------------------------
# 4. Upstream Workarounds (ESP-IDF 6.1)
# ------------------------------------------------------------------------------
# GCC 15 (ESP-IDF 6.x) reports a false-positive -Warray-bounds in NimBLE's
# ble_store_nvs.c with CONFIG_BT_NIMBLE_MAX_BONDS=1: the bond-order check reads
# [i+1] but only runs when more than one bond is stored. IDF builds with -Werror.
# The bt component is compiled even when BLE OTA is off (e.g. ESP32-S2).
idf_build_get_property(build_components BUILD_COMPONENTS)
if("bt" IN_LIST build_components)
    idf_component_get_property(bt_dir bt COMPONENT_DIR)
    set_source_files_properties(
        "${bt_dir}/host/nimble/nimble/nimble/host/store/config/src/ble_store_nvs.c"
        DIRECTORY "${bt_dir}"
        PROPERTIES COMPILE_OPTIONS "-Wno-error=array-bounds"
    )
endif()

# src/mbedtls_user_config.h compiles out mbedTLS's software AES, so mbedTLS's
# cmac.h falls back to the 3DES block size and IDF's CMAC driver then redefines
# MBEDTLS_CMAC_MAX_BLOCK_SIZE as 16 (correct for AES). The redefinition is benign.
if(CONFIG_MBEDTLS_HARDWARE_AES AND CONFIG_MBEDTLS_CMAC_C)
    idf_component_get_property(mbedtls_dir mbedtls COMPONENT_DIR)
    set_source_files_properties(
        "${mbedtls_dir}/port/psa_driver/esp_mac/psa_crypto_driver_esp_cmac.c"
        TARGET_DIRECTORY tfpsacrypto
        PROPERTIES COMPILE_OPTIONS "-Wno-error"
    )
endif()

# The builtin CMAC goes with software AES. wpa_supplicant's PSA-based
# omac1_aes_128() is guarded by the builtin's MBEDTLS_CMAC_C macro instead of
# PSA_WANT_ALG_CMAC, so build the supplicant's own CMAC over AES-ECB (what IDF
# uses when CONFIG_MBEDTLS_CMAC_C=n, which ESP_WIFI_MBEDTLS_CRYPTO forbids).
if("wpa_supplicant" IN_LIST build_components AND CONFIG_ESP_WIFI_MBEDTLS_CRYPTO
   AND CONFIG_MBEDTLS_HARDWARE_AES AND CONFIG_MBEDTLS_CMAC_C)
    idf_component_get_property(wpa_lib wpa_supplicant COMPONENT_LIB)
    idf_component_get_property(wpa_dir wpa_supplicant COMPONENT_DIR)
    target_sources(${wpa_lib} PRIVATE "${wpa_dir}/src/crypto/aes-omac1.c")
endif()

# ------------------------------------------------------------------------------
# 5. libc
# ------------------------------------------------------------------------------
# Integer-only printf/scanf (picolibc's PICOLIBC_INTEGER_PRINTF_SCANF), the
# picolibc counterpart of newlib's nano formatting: nothing here formats floats.
if(CONFIG_LIBC_PICOLIBC)
    idf_build_get_property(project_elf EXECUTABLE)
    target_link_options(${project_elf} PRIVATE
        "-Wl,--defsym=vfprintf=__i_vfprintf"
        "-Wl,--defsym=vfscanf=__i_vfscanf"
    )
endif()

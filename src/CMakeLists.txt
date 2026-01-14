# Feature toggles (can be set from -D on the CMake command line)
option(ENABLE_BLE_OTA  "Build BLE OTA support"  OFF)
option(ENABLE_WIFI_OTA "Build WiFi OTA support" ON)

# Base sources/requirements (always needed)
set(ota_srcs
    "main.cpp"
    "nvs_config.cpp"
    "ota_processor.cpp"
    "utils.cpp"
)

set(ota_reqs
    nvs_flash
    app_update
    spi_flash
)

# WiFi OTA
if(ENABLE_WIFI_OTA)
    list(APPEND ota_srcs "wifi_app.cpp" "net_ota.cpp")
    list(APPEND ota_reqs esp_wifi esp_event esp_netif esp_phy lwip mbedtls)
endif()

# BLE OTA
if(ENABLE_BLE_OTA)
    list(APPEND ota_srcs "ble_ota.cpp")
    list(APPEND ota_reqs "esp-nimble-cpp")
endif()

idf_component_register(
    SRCS         ${ota_srcs}
    INCLUDE_DIRS "."
    REQUIRES     ${ota_reqs}
)

# Export macros to the code
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    $<$<BOOL:${ENABLE_WIFI_OTA}>:USE_WIFI_OTA>
    $<$<BOOL:${ENABLE_BLE_OTA}>:USE_BLE_OTA>
)

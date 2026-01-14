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

# Apply the dynamic name derived in Step 1
project(${OTA_PROJECT_NAME} VERSION "${PROJECT_VER}")

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        nvs_flash 
        esp_wifi 
        esp_event 
        esp_netif 
        esp_phy 
        lwip 
        mbedtls 
        esp-nimble-cpp
        app_update 
        spi_flash
)
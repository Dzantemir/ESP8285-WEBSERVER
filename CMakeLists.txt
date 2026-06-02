cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXCLUDE_COMPONENTS app_update bootloader_support coap console esp-tls esp-wolfssl esp8266 esp_common esp_event esp_gdbstub esp_http_client esp_https_ota esp_ringbuf freemodbus freertos heap http_parser jsmn json libsodium log lwip mbedtls mdns mqtt newlib nvs_flash openssl protobuf-c protocomm spi_flash spi_ram tcp_transport tcpip_adapter vfs wear_levelling wifi_provisioning wpa_supplicant)
project(esp8285_smart_ap)



# ESP8266 Tools: partition bin links -- BEGIN
if(CONFIG_PARTITION_TABLE_CUSTOM)
    esptool_py_flash_project_args(storage 0xB0000 "${CMAKE_CURRENT_SOURCE_DIR}/data_spiffs.bin" FLASH_IN_PROJECT)
endif()
# ESP8266 Tools: partition bin links -- END

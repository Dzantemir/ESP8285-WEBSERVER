cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp8285_smart_ap)









# ESP8266 Tools: partition bin links -- BEGIN
if(CONFIG_PARTITION_TABLE_CUSTOM)
    esptool_py_flash_project_args(storage 0xB0000 "${CMAKE_CURRENT_SOURCE_DIR}/data_spiffs.bin" FLASH_IN_PROJECT)
endif()
# ESP8266 Tools: partition bin links -- END

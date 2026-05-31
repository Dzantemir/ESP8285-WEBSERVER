

# Add SD SPI driver only when SD card storage is enabled
if(CONFIG_SMART_AP_STORAGE_SD OR CONFIG_SMART_AP_STORAGE_SPIFFS_SD)
  set(COMPONENT_REQUIRES sd_spi_driver)
endif()

idf_component_register(
    SRCS "buzzer.c" "dns_server.c" "main.c" "orchestrator.c" "power_mgmt.c" "storage.c" "web_server.c" "wifi_ap.c"
    INCLUDE_DIRS include
    REQUIRES ${COMPONENT_REQUIRES}
)

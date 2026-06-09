idf_component_register(
    SRCS "buzzer.c" "dns_server.c" "main.c" "orchestrator.c" "power_mgmt.c" "storage.c" "web_server.c" "wifi_ap.c"
    INCLUDE_DIRS include
    REQUIRES nvs_flash spiffs pthread
    PRIV_REQUIRES sd_spi_driver esp_http_server
)

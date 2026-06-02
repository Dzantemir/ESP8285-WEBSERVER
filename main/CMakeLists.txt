# Зависимости указываем напрямую в idf_component_register(), а не через
# list(APPEND) + условный if(CONFIG_...). Причина: CMake обрабатывает
# этот файл в два прохода, и на первом проходе CONFIG_ переменные ещё
# пустые. idf_component_register() фиксирует REQUIRES по результатам
# первого прохода, и list(APPEND) на втором проходе уже не добавляет
# include-пути. Условная компиляция в C-коде (#if STORAGE_USES_SPIFFS,
# #if STORAGE_USES_SD) гарантирует, что лишний include-путь не вызовет
# ошибок — неиспользуемые заголовки просто не включаются.

idf_component_register(
    SRCS "buzzer.c" "dns_server.c" "main.c" "orchestrator.c" "power_mgmt.c" "storage.c" "web_server.c" "wifi_ap.c"
    INCLUDE_DIRS include
    REQUIRES esp_http_server nvs_flash spiffs pthread
    PRIV_REQUIRES sd_spi_driver
)
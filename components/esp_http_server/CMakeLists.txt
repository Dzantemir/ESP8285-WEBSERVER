# ESP8266 RTOS SDK adaptation of esp_http_server
# Ported from ESP-IDF with the following changes:
# - esp_timer removed from priv_req (part of esp8266 component on ESP8266)
# - Port directory changed from esp32 to esp8266
# - HTTPD_CRYPTO_SRC removed (PSA Crypto replaced with mbedtls SHA-1)


idf_component_register(SRCS "src/httpd_main.c"
                            "src/httpd_parse.c"
                            "src/httpd_sess.c"
                            "src/httpd_txrx.c"
                            "src/httpd_uri.c"
                            "src/httpd_ws.c"
                            "src/util/ctrl_sock.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "src/util" "src/port/esp8266"
                    REQUIRES http_parser esp_event
                    PRIV_REQUIRES mbedtls lwip
)
idf_component_register(SRCS "main.c" "hid_dev.c" "hid_parse.c" "web.c" "padflash.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event
                                     esp_http_server json esp_timer mbedtls esp_partition espressif__esp-serial-flasher
                       EMBED_TXTFILES "index.html")

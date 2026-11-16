idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_wifi esp_event esp_netif
                                     esp_timer nvs_flash mqtt esp_hw_support)

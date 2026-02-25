idf_component_register(SRCS "main.cpp"
                    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif driver esp_common esp_adc esp_timer
                    INCLUDE_DIRS ".")

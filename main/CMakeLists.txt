idf_component_register(
    SRCS "app_main.c"
         "hcsr04.c"
         "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_http_server esp_wifi tcpip_adapter
)

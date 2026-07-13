idf_component_register(
    SRCS "app_main.c"
         "telemetry.c"
         "dyp_a22.c"
         "hcsr04.c"
         "transport.c"
         "wifi_transport.c"
         "cellular_transport.c"
         "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_http_server esp_wifi tcpip_adapter esp_http_client
)

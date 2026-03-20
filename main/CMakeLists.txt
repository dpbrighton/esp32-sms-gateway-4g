idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        a7670e
        sms_store
        web_server
        rest_api
        mqtt_gateway
        webhook
        config
        ota_update
        tcp_log
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        log
)

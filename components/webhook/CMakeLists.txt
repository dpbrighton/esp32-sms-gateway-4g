idf_component_register(
    SRCS "webhook.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp-tls esp_http_client log sms_store config freertos
)

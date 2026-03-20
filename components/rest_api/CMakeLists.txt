idf_component_register(
    SRCS "rest_api.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server log sms_store a7670e config ota_update
)

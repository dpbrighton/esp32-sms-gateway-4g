idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server log sms_store a7670e config
)

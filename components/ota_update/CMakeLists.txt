idf_component_register(
    SRCS "ota_update.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client log esp_system app_update
)

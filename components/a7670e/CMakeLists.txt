idf_component_register(
    SRCS "a7670e.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio log freertos sms_store
)

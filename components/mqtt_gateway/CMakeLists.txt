idf_component_register(
    SRCS "mqtt_gateway.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        sms_store
        a7670e
        config
        mqtt
        cjson
        freertos
)


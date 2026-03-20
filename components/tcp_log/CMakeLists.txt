idf_component_register(
    SRCS "tcp_log.c"
    INCLUDE_DIRS "."
    REQUIRES
        log
        freertos
        lwip
)

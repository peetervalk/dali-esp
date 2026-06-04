idf_component_register(
    SRCS
        "dali_ringbuf.c"
        "dali_phy.c"
        "dali_scheduler.c"
        "dali_protocol.c"
        "dali_diag.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        freertos
)

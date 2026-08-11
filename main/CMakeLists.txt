idf_component_register(
    SRCS
        "main.c"
        "dali_cli.c"
        "dali_diag.c"
    INCLUDE_DIRS "."
    REQUIRES
        dali
        esp_driver_uart
)

idf_component_register(
    SRCS "p4scan_lcm_display.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_hw_support esp_driver_ledc esp_driver_gpio freertos log
)

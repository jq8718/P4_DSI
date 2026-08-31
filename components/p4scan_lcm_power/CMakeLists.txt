idf_component_register(
    SRCS "p4scan_lcm_power.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio freertos log
)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES p4scan_lcm_power p4scan_lcm_display p4scan_touch
)

idf_component_register(
    SRCS "app.c"
    INCLUDE_DIRS "include"
    REQUIRES board battery_monitor event host_link haptics input_strain ui led_ring ble_dial
)

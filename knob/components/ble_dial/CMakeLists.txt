idf_component_register(
    SRCS "ble_dial.c" "ble_dial_hid.c"
    INCLUDE_DIRS "include"
    REQUIRES event haptics bt
)

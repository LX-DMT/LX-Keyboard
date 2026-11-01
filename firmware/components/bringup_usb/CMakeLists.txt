idf_component_register(SRCS "bringup_usb.c" "usb_keys.c" INCLUDE_DIRS "include"
    REQUIRES esp_tinyusb tinyusb esp_hw_support esp_timer input_core hid_mouse protocol)

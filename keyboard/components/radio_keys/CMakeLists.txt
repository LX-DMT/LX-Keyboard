idf_component_register(SRCS "radio_keys.c" INCLUDE_DIRS "include"
    REQUIRES protocol input_core bringup_usb hid_mouse network_link esp_wifi esp_netif esp_event esp_timer nvs_flash)

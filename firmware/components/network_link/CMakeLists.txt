idf_component_register(SRCS "network_base.c" "network_control.c" "network_discovery.c" "wifi_keys.c" "link_control.c" "device_setup.c" "link_auth.c" INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash lwip protocol input_core hid_mouse bringup_usb mbedtls)

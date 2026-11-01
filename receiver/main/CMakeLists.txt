idf_component_register(SRCS "main.c" "status_led.c" INCLUDE_DIRS "."
    REQUIRES bringup_usb radio_keys network_link protocol nvs_flash esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer esp_hw_support esp_psram spi_flash)

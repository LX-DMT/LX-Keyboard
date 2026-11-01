idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_lcd esp_timer freertos lvgl driver
)

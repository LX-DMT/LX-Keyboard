idf_component_register(
    SRCS "battery_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES board event esp_adc driver freertos
)

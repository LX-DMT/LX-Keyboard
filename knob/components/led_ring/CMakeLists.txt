idf_component_register(
    SRCS "led_ring.c"
    INCLUDE_DIRS "include"
    REQUIRES board driver led_strip freertos
)

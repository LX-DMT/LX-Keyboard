idf_component_register(
    SRCS "host_link.c"
    INCLUDE_DIRS "include"
    REQUIRES board event driver freertos
)

idf_component_register(
    SRCS "hx711.c" "input_strain.c" "strain_cal_text.c"
    INCLUDE_DIRS "include"
    REQUIRES board event haptics driver freertos nvs_flash project_build_id
)

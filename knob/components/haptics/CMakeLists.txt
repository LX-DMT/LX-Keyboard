idf_component_register(
    SRCS "as5047_encoder.cpp" "haptic_profiles.c" "haptics_engine.cpp" "motor_foc_nvs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board event esp_simplefoc driver freertos esp_timer nvs_flash project_build_id
)

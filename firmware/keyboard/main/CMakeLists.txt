idf_component_register(SRCS "main.c" "board.c" "board_audio.c" "battery_led.c" "usb_host_base.c" "ble_keys.c" "knob_link.c" "user_controls.c" "voice_capture.c" "voice_duplex.c" "wake_word.c" "asr_baidu.c" "voice_opus.c" "chat_protocol.c" "chat_client.c" "chat_fixture.c" "chat_official.c" "activation_audio.c"
    "power_policy.c" "battery_policy.c" "activation_ui.c" "chat_uplink.c" "custom_keys.c" "lighting_settings.c"
    EMBED_FILES "activation_audio/clips.bin" "power_audio/startup.pcm" "power_audio/shutdown.pcm"
    INCLUDE_DIRS "."
    REQUIRES bringup_usb input_core radio_keys network_link protocol hid_mouse esp_driver_gpio esp_driver_spi esp_driver_i2c esp_driver_i2s
        esp_driver_uart esp_driver_ledc esp_timer esp_psram spi_flash nvs_flash
        esp_codec_dev led_strip usb usb_host_hid bt esp-sr esp_http_client mbedtls cjson esp_audio_codec esp_websocket_client)

#pragma once

/**
 * @file as5047_encoder.hpp
 * @brief AS5047 磁编码器，实现 SimpleFOC Sensor 接口
 */

#include "common/base_classes/Sensor.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

class As5047Encoder : public Sensor {
public:
    As5047Encoder(spi_host_device_t host, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t cs);

    void init() override;
    float getSensorAngle() override;
    uint16_t readRawAngle();

private:
    uint16_t readRegister(uint16_t reg);
    static uint8_t evenParity(uint16_t value);

    spi_host_device_t m_host;
    gpio_num_t m_mosi;
    gpio_num_t m_miso;
    gpio_num_t m_sclk;
    gpio_num_t m_cs;
    spi_device_handle_t m_dev;
    bool m_installed;
};

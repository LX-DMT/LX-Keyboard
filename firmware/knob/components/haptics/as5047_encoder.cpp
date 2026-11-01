/**
 * @file as5047_encoder.cpp
 * @brief AS5047 磁编码器 SPI 驱动（SimpleFOC Sensor）
 *
 * 1MHz SPI mode 1，14 位角度；协议对齐原 Arduino MagneticSensorSPI(AS5047_SPI)。
 */

#include "as5047_encoder.hpp"

#include "common/foc_utils.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "as5047";

static constexpr uint16_t kAngleRegister = 0x3FFF;
static constexpr float kCountsPerRev = 16384.0f;

As5047Encoder::As5047Encoder(spi_host_device_t host, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t cs)
    : m_host(host),
      m_mosi(mosi),
      m_miso(miso),
      m_sclk(sclk),
      m_cs(cs),
      m_dev(nullptr),
      m_installed(false)
{
}

uint8_t As5047Encoder::evenParity(uint16_t value)
{
    uint8_t count = 0;
    for (int i = 0; i < 16; i++) {
        count += value & 0x1;
        value >>= 1;
    }
    return count & 0x1;
}

uint16_t As5047Encoder::readRegister(uint16_t reg)
{
    uint16_t command = reg | (1 << 14);
    command |= ((uint16_t)evenParity(command) << 15);

    /* One CS window: command frame then data frame (matches Arduino transfer16 x2). */
    uint8_t tx[4] = {
        (uint8_t)(command >> 8),
        (uint8_t)(command & 0xFF),
        0,
        0,
    };
    uint8_t rx[4] = {};

    spi_transaction_t trans = {};
    trans.length = 32;
    trans.tx_buffer = tx;
    trans.rx_buffer = rx;
    ESP_ERROR_CHECK(spi_device_transmit(m_dev, &trans));

    uint16_t value = ((uint16_t)rx[2] << 8) | rx[3];
    return value & 0x3FFF;
}

void As5047Encoder::init()
{
    if (m_installed) {
        return;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = m_mosi,
        .miso_io_num = m_miso,
        .sclk_io_num = m_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    esp_err_t ret = spi_bus_initialize(m_host, &buscfg, SPI_DMA_DISABLED);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, , TAG, "SPI bus init failed: %s", esp_err_to_name(ret));

    spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 1,
        .cs_ena_pretrans = 1,
        .clock_speed_hz = 1 * 1000 * 1000,
        .spics_io_num = m_cs,
        .queue_size = 1,
    };

    ret = spi_bus_add_device(m_host, &devcfg, &m_dev);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, , TAG, "SPI add device failed: %s", esp_err_to_name(ret));

    m_installed = true;
    Sensor::init();

    /* Flush pipeline; first sample after power-up can be invalid. */
    (void)readRegister(kAngleRegister);
    esp_rom_delay_us(50);
    (void)readRegister(kAngleRegister);

    const uint16_t raw = readRegister(kAngleRegister);
    ESP_LOGI(TAG, "AS5047 ready, raw=0x%04X (%.2f deg)", raw, (raw * 360.0f) / kCountsPerRev);
}

uint16_t As5047Encoder::readRawAngle()
{
    return readRegister(kAngleRegister);
}

float As5047Encoder::getSensorAngle()
{
    return (readRawAngle() / kCountsPerRev) * _2PI;
}

#pragma once

/**
 * @file board_pins.h
 * @brief LX Dial 硬件引脚与总线定义（ESP32-S3）
 *
 * 集中定义电机 PWM、磁编码器 SPI、圆屏 SPI、HX711、灯环、主机 UART 等引脚，
 * 供 haptics / display / input_strain / host_link 等组件引用。
 */

#include "driver/spi_master.h"
#include "driver/uart.h"

/* 电机三相 6-PWM */
#define BOARD_MOTOR_PIN_UH 15
#define BOARD_MOTOR_PIN_UL 9
#define BOARD_MOTOR_PIN_VH 17
#define BOARD_MOTOR_PIN_VL 5
#define BOARD_MOTOR_PIN_WH 8
#define BOARD_MOTOR_PIN_WL 6

/* AS5047 磁编码器，SPI2 */
#define BOARD_ENCODER_SPI_HOST   SPI2_HOST
#define BOARD_ENCODER_PIN_MOSI   11
#define BOARD_ENCODER_PIN_MISO   13
#define BOARD_ENCODER_PIN_SCLK   12
#define BOARD_ENCODER_PIN_CS     10

/* GC9A01 圆屏，SPI3 */
#define BOARD_LCD_SPI_HOST       SPI3_HOST
#define BOARD_LCD_PIN_MOSI       3
#define BOARD_LCD_PIN_SCLK       2
#define BOARD_LCD_PIN_DC         21
#define BOARD_LCD_PIN_RST        16
#define BOARD_LCD_WIDTH          240
#define BOARD_LCD_HEIGHT         240

/* 电池电压采样：GPIO7，10K+10K 分压（VBAT/2） */
#define BOARD_BAT_ADC_GPIO       7

/* HX711 应变片 */
#define BOARD_STRAIN_PIN_DOUT    40
#define BOARD_STRAIN_PIN_SCK     39

/* WS2812 灯环 */
#define BOARD_LED_RING_PIN_DATA  18
#define BOARD_LED_RING_PIN_PWR   14
#define BOARD_LED_RING_COUNT     50

/* RF/外设使能（Surface Dial 模式高电平有效） */
#define BOARD_RF_ENABLE_PIN      45

/* 主机 UART：UART0 TX/RX（GPIO43/44），与上位机协议通信 */
#define BOARD_HOST_UART_NUM      UART_NUM_0
#define BOARD_HOST_UART_TX       43
#define BOARD_HOST_UART_RX       44
#define BOARD_HOST_UART_BAUD     115200

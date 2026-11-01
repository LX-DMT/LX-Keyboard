/* Board wiring and SPI timing checked against the schematic and old BSP only. */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "board.h"
#include "battery_led.h"
#include "activation_ui.h"
#include "power_policy.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static spi_device_handle_t s_scan;
static i2c_master_bus_handle_t s_audio_bus, s_power_bus;
static i2c_master_dev_handle_t s_ina;
static led_strip_handle_t s_led;
static SemaphoreHandle_t s_led_lock;
static SemaphoreHandle_t s_knob_lock;
static atomic_bool s_knob_on=true;
static bool s_led_powered;
/* Match the reference: 10 s dark, 1.5 s up, 1.5 s down, 50% peak.
 * Controls' existing 20 ms tick updates hardware PWM; no fade ISR or task. */
#define SIDE_WAIT_US 10000000LL
#define SIDE_HALF_US 1500000LL
#define SIDE_PERIOD_US (SIDE_WAIT_US+2*SIDE_HALF_US)
#define SIDE_MAX_DUTY 512U
static bool s_side_ready,s_side_allowed;
static int64_t s_side_epoch;
static unsigned s_side_duty,s_side_phase_ms,s_side_errors;
static esp_err_t s_side_result;
static const char *TAG = "board";

static esp_err_t output(int pin, int level, bool open_drain)
{
    gpio_set_level(pin, level);
    const gpio_config_t config = {.pin_bit_mask=1ULL<<pin,
        .mode=open_drain?GPIO_MODE_OUTPUT_OD:GPIO_MODE_OUTPUT};
    return gpio_config(&config);
}
static esp_err_t i2c_bus(int port, int sda, int scl, i2c_master_bus_handle_t *handle)
{
    i2c_master_bus_config_t config = {.i2c_port=port, .sda_io_num=sda, .scl_io_num=scl,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7, .flags.enable_internal_pullup=true};
    return i2c_new_master_bus(&config, handle);
}
static void side_led_init(void)
{
    const ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,
        .duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_1,
        .freq_hz=1000,.clk_cfg=LEDC_AUTO_CLK};
    const ledc_channel_config_t channel={.gpio_num=PIN_SIDE_LED,
        .speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,
        .timer_sel=LEDC_TIMER_1,.duty=0,.hpoint=0};
    s_side_result=ledc_timer_config(&timer);
    if(s_side_result==ESP_OK)s_side_result=ledc_channel_config(&channel);
    s_side_ready=s_side_result==ESP_OK;
    if(!s_side_ready) {
        s_side_errors++;gpio_reset_pin(PIN_SIDE_LED);output(PIN_SIDE_LED,0,false);
        /* Optional lighting must not prevent normal typing from starting. */
        printf("LX_SIDE_LED init=%s gpio=42\n",esp_err_to_name(s_side_result));
    }
}
/* All runtime LEDC writes share the existing lighting mutex. */
static void side_led_duty(unsigned duty)
{
    if(!s_side_ready || (s_side_duty==duty && s_side_result==ESP_OK))return;
    s_side_result=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,duty);
    if(s_side_result==ESP_OK)s_side_result=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);
    if(s_side_result==ESP_OK)s_side_duty=duty;else s_side_errors++;
}
static void side_led_off(void)
{
    s_side_allowed=false;s_side_epoch=0;s_side_phase_ms=0;side_led_duty(0);
}
void board_side_led_tick(void)
{
    if(xSemaphoreTake(s_led_lock,pdMS_TO_TICKS(20))!=pdTRUE)return;
    if(!s_side_ready || power_policy_dark() || power_policy_shutting_down() || battery_led_blocked()) {
        side_led_off();
    } else {
        int64_t now=esp_timer_get_time();
        if(!s_side_allowed) { s_side_epoch=now;s_side_allowed=true; }
        int64_t phase=(now-s_side_epoch)%SIDE_PERIOD_US;
        s_side_phase_ms=(unsigned)(phase/1000);
        unsigned duty=0;
        if(phase>=SIDE_WAIT_US) {
            int64_t ramp=phase-SIDE_WAIT_US;
            duty=(unsigned)(SIDE_MAX_DUTY*(ramp<=SIDE_HALF_US?ramp:2*SIDE_HALF_US-ramp)/SIDE_HALF_US);
        }
        side_led_duty(duty);
    }
    xSemaphoreGive(s_led_lock);
}
void board_side_led_stats(void)
{
    if(xSemaphoreTake(s_led_lock,pdMS_TO_TICKS(200))!=pdTRUE) {
        puts("LX_SIDE_LED busy=1");return;
    }
    bool ready=s_side_ready,allowed=s_side_allowed;
    unsigned duty=s_side_duty,phase=s_side_phase_ms,errors=s_side_errors;
    unsigned hardware=ready?ledc_get_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0):0;
    esp_err_t rc=s_side_result;xSemaphoreGive(s_led_lock);
    printf("LX_SIDE_LED gpio=42 ready=%u allowed=%u duty=%u hw_duty=%u max_duty=512 phase_ms=%u wait_ms=10000 half_ms=1500 period_ms=13000 pwm_hz=1000 errors=%u result=%s visual=not_measured\n",
        ready,allowed,duty,hardware,phase,errors,esp_err_to_name(rc));
}
esp_err_t board_init(bool usb_host)
{
    ESP_RETURN_ON_ERROR(output(PIN_POWER,1,false),TAG,"power");
    ESP_RETURN_ON_ERROR(output(PIN_KNOB_POWER,0,false),TAG,"knob power");
    ESP_RETURN_ON_ERROR(gpio_input_enable(PIN_KNOB_POWER),TAG,"knob readback");
    ESP_RETURN_ON_ERROR(output(PIN_AUDIO_POWER_OFF,1,false),TAG,"mute");
    ESP_RETURN_ON_ERROR(gpio_input_enable(PIN_AUDIO_POWER_OFF),TAG,"mute readback");
    /* USB_SD_EN is active low; only wired USB mode enables the SD path. */
    ESP_RETURN_ON_ERROR(output(PIN_USB_SD_EN,usb_host?1:0,false),TAG,"USB SD enable");
    ESP_RETURN_ON_ERROR(gpio_input_enable(PIN_USB_SD_EN),TAG,"USB SD readback");
    ESP_RETURN_ON_ERROR(output(PIN_USB_MODE,usb_host?1:0,false),TAG,"USB role");
    ESP_RETURN_ON_ERROR(gpio_input_enable(PIN_USB_MODE),TAG,"USB role readback");
    ESP_RETURN_ON_ERROR(output(PIN_RGB_POWER,1,true),TAG,"RGB off");
    ESP_RETURN_ON_ERROR(output(PIN_SIDE_LED,0,false),TAG,"side LED off");
    ESP_RETURN_ON_ERROR(output(PIN_SCAN_PL,0,false),TAG,"PL");
    ESP_RETURN_ON_ERROR(output(PIN_SCAN_CE,0,false),TAG,"CE");
    spi_bus_config_t bus = {.mosi_io_num=-1, .miso_io_num=PIN_SCAN_MISO,
        .sclk_io_num=PIN_SCAN_CLK, .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=16};
    /* Eleven bytes fit in the FIFO; avoid a DMA bounce allocation on every scan. */
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_DISABLED),TAG,"scan bus");
    spi_device_interface_config_t device = {.clock_speed_hz=1000000, .mode=2,
        .spics_io_num=-1, .queue_size=1};
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST,&device,&s_scan),TAG,"scan device");
    uart_config_t uart = {.baud_rate=115200, .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT};
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_1,1024,0,0,NULL,0),TAG,"UART");
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM_1,&uart),TAG,"UART config");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM_1,PIN_KNOB_TX,PIN_KNOB_RX,-1,-1),TAG,"UART pins");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(PIN_KNOB_RX,GPIO_PULLUP_ONLY),TAG,"UART RX idle");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(PIN_KNOB_TX,GPIO_PULLUP_ONLY),TAG,"UART TX idle");
    ESP_RETURN_ON_ERROR(i2c_bus(0,PIN_POWER_SDA,PIN_POWER_SCL,&s_power_bus),TAG,"power I2C");
    ESP_RETURN_ON_ERROR(i2c_bus(1,PIN_AUDIO_SDA,PIN_AUDIO_SCL,&s_audio_bus),TAG,"audio I2C");
    i2c_device_config_t ina = {.dev_addr_length=I2C_ADDR_BIT_LEN_7, .device_address=0x40,
        .scl_speed_hz=100000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_power_bus,&ina,&s_ina),TAG,"INA219");
    s_led_lock=xSemaphoreCreateMutex();
    s_knob_lock=xSemaphoreCreateMutex();
    if(!s_led_lock || !s_knob_lock) return ESP_ERR_NO_MEM;
    /* Continuous shunt + bus, 32 V range, 320 mV shunt, 12-bit samples. */
    const uint8_t ina_config[]={0,0x39,0x9f};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_ina,ina_config,sizeof(ina_config),50),TAG,"INA config");
    side_led_init();
    return ESP_OK;
}
esp_err_t board_power_read(uint16_t *mv,int16_t *ma)
{
    uint8_t reg=2,bytes[2];
    esp_err_t result=i2c_master_transmit_receive(s_ina,&reg,1,bytes,2,30);
    if(result!=ESP_OK) return result;
    uint16_t bus=((uint16_t)bytes[0]<<8)|bytes[1];
    if(!(bus&2) || (bus&1)) return ESP_ERR_INVALID_STATE;
    *mv=(bus>>3)*4;
    reg=1;result=i2c_master_transmit_receive(s_ina,&reg,1,bytes,2,30);
    /* The physical shunt is 10 mOhm; 10 uV/LSB therefore equals 1 mA/LSB.
     * Keep sensor polarity; the knob's public protocol inverts it for display. */
    if(result==ESP_OK) *ma=(int16_t)(((uint16_t)bytes[0]<<8)|bytes[1]);
    return result;
}
esp_err_t board_scan(uint8_t data[BOARD_SCAN_BYTES])
{
    spi_transaction_t transfer = {.length=BOARD_SCAN_BYTES*8, .rx_buffer=data};
    gpio_set_level(PIN_SCAN_PL,1);
    esp_rom_delay_us(10);
    esp_err_t result = spi_device_polling_transmit(s_scan,&transfer);
    gpio_set_level(PIN_SCAN_PL,0);
    return result;
}
i2c_master_bus_handle_t board_audio_bus(void) { return s_audio_bus; }
void board_print_i2c(void)
{
    esp_err_t codec = i2c_master_probe(s_audio_bus,0x18,50);
    esp_err_t ina = i2c_master_probe(s_power_bus,0x40,50);
    uint8_t reg=2, bytes[2]={0};
    esp_err_t voltage = i2c_master_transmit_receive(s_ina,&reg,1,bytes,2,50);
    unsigned mv = (((unsigned)bytes[0]<<8 | bytes[1])>>3)*4;
    printf("LX_M0 i2c codec_0x18=%s ina219_0x40=%s bus_mv=%u read=%s\n",
           esp_err_to_name(codec),esp_err_to_name(ina),mv,esp_err_to_name(voltage));
    printf("LX_M0 audio_adc_0x40=%s bus=1\n",
           esp_err_to_name(i2c_master_probe(s_audio_bus,0x40,50)));
}
void board_print_uart(void)
{
    size_t buffered=0;
    uart_get_buffered_data_len(UART_NUM_1,&buffered);
    uint8_t bytes[256];
    int received=uart_read_bytes(UART_NUM_1,bytes,sizeof(bytes),0);
    printf("LX_M0 uart tx=%d rx=%d baud=115200 buffered=%u received=%d\n",
           PIN_KNOB_TX,PIN_KNOB_RX,(unsigned)buffered,received);
    /* A valid HOST_STATS fixture verifies TX without changing knob settings. */
    uint8_t frame[]={0x4c,0x58,1,0x80,3,25,50,75,0};
    for (unsigned i=2;i<8;i++) {
        frame[8]^=frame[i];
        for (unsigned bit=0;bit<8;bit++) frame[8]=(frame[8]&0x80)?(frame[8]<<1)^7:frame[8]<<1;
    }
    int sent=board_knob_write(frame,sizeof(frame));
    printf("LX_M0 uart_stats_written=%d display_verification=not_measured\n",sent);
}
bool board_knob_powered(void) { return atomic_load(&s_knob_on); }
int board_knob_power_level(void) { return gpio_get_level(PIN_KNOB_POWER); }
int board_knob_write(const void *data,size_t size)
{
    if(xSemaphoreTake(s_knob_lock,pdMS_TO_TICKS(50))!=pdTRUE) return -1;
    int n=board_knob_powered() && !power_policy_dark() && !power_policy_shutting_down()
        ?uart_write_bytes(UART_NUM_1,data,size):-1;
    xSemaphoreGive(s_knob_lock);return n;
}
esp_err_t board_knob_power(bool on)
{
    if(xSemaphoreTake(s_knob_lock,pdMS_TO_TICKS(100))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t rc=ESP_OK;
    if(on && power_policy_shutting_down()) { rc=ESP_ERR_INVALID_STATE;goto done; }
    if(on==board_knob_powered())goto done;
    if(!on) {
        rc=uart_wait_tx_done(UART_NUM_1,pdMS_TO_TICKS(50));
        if(rc!=ESP_OK)goto done;
        /* No idle-high TX or pullups feeding an unpowered knob. */
        uart_disable_rx_intr(UART_NUM_1);
        gpio_set_direction(PIN_KNOB_TX,GPIO_MODE_INPUT);
        gpio_set_pull_mode(PIN_KNOB_TX,GPIO_FLOATING);
        gpio_set_pull_mode(PIN_KNOB_RX,GPIO_FLOATING);
        gpio_set_level(PIN_KNOB_POWER,1);
        atomic_store(&s_knob_on,false);
    } else {
        gpio_set_level(PIN_KNOB_POWER,0);
        rc=uart_set_pin(UART_NUM_1,PIN_KNOB_TX,PIN_KNOB_RX,-1,-1);
        if(rc!=ESP_OK) { gpio_set_level(PIN_KNOB_POWER,1);goto done; }
        gpio_set_pull_mode(PIN_KNOB_TX,GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PIN_KNOB_RX,GPIO_PULLUP_ONLY);
        uart_flush_input(UART_NUM_1);uart_enable_rx_intr(UART_NUM_1);
        atomic_store(&s_knob_on,true);
    }
done:
    xSemaphoreGive(s_knob_lock);return rc;
}
static esp_err_t led_rgb(uint8_t r,uint8_t g,uint8_t b,uint8_t battery_v)
{
    if(power_policy_shutting_down()) {
        gpio_set_level(PIN_RGB_POWER,1);side_led_off();
        s_led_powered=false;
        return ESP_OK;
    }
    bool idle=power_policy_dark();
    if(idle)r=g=b=0;
    if (!s_led) {
        led_strip_config_t config={.strip_gpio_num=PIN_RGB_DATA,.max_leds=BOARD_LED_COUNT,
            .led_pixel_format=LED_PIXEL_FORMAT_GRB,.led_model=LED_MODEL_WS2812};
        /* Encode all 96 pixels before transmission: SPI DMA needs no ISR
         * refill midway through the LED chain. SPI2 remains dedicated to
         * key scanning; SPI3 has only MOSI connected to the existing pin. */
        led_strip_spi_config_t spi={.clk_src=SPI_CLK_SRC_DEFAULT,
            .spi_bus=SPI3_HOST,.flags.with_dma=true};
        esp_err_t result=led_strip_new_spi_device(&config,&spi,&s_led);
        if(result!=ESP_OK) { battery_led_result(result,NULL);return result; }
        printf("LX_LED transport=spi_dma host=3 frame_bytes=%u pixels=%u\n",
               (unsigned)BOARD_LED_COUNT*9,(unsigned)BOARD_LED_COUNT);
    }
    uint8_t pixels[BOARD_LED_COUNT][3];
    for(unsigned i=0;i<BOARD_LED_COUNT;i++) { pixels[i][0]=r;pixels[i][1]=g;pixels[i][2]=b; }
    battery_led_frame_t frame;
    if(!idle)activation_ui_overlay(pixels,BOARD_LED_COUNT);
    battery_led_overlay(pixels,battery_v,&frame);
    frame.idle=idle;
    if(!s_led_powered) {
        gpio_set_level(PIN_RGB_POWER,0);
        /* Full power-up/reset interval before the first data frame. */
        vTaskDelay(pdMS_TO_TICKS(2)+1);s_led_powered=true;
    }
    if(frame.blocked || idle)side_led_off();
    for(unsigned i=0;i<BOARD_LED_COUNT;i++) {
        esp_err_t result=led_strip_set_pixel(s_led,i,pixels[i][0],pixels[i][1],pixels[i][2]);
        if(result!=ESP_OK) { battery_led_result(result,&frame);return result; }
    }
    esp_err_t result=led_strip_refresh(s_led);
    /* SPI ends in a low bit. Keep a complete WS2812 latch/reset interval
     * under the output mutex, including back-to-back diagnostic frames. */
    if(result==ESP_OK)esp_rom_delay_us(300);
    /* The four status pixels stay on even when the key effect is black. */
    battery_led_result(result,&frame);
    return result;
}
esp_err_t board_led_rgb(uint8_t r,uint8_t g,uint8_t b,uint8_t battery_v)
{
    if(xSemaphoreTake(s_led_lock,pdMS_TO_TICKS(200))!=pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result=led_rgb(r,g,b,battery_v);xSemaphoreGive(s_led_lock);return result;
}
void board_led_test(void)
{
    if(xSemaphoreTake(s_led_lock,pdMS_TO_TICKS(200))!=pdTRUE) return;
    esp_err_t result=led_rgb(2,2,2,64);
    vTaskDelay(pdMS_TO_TICKS(150));
    led_rgb(0,0,0,64);xSemaphoreGive(s_led_lock);
    printf("LX_M0 led_transfer=%s visual_verification=not_measured\n",esp_err_to_name(result));
}
esp_err_t board_lights_off(void)
{
    if(xSemaphoreTake(s_led_lock,pdMS_TO_TICKS(200))!=pdTRUE) return ESP_ERR_TIMEOUT;
    /* DMA has completed under the same mutex. Drive data low before removing
     * the shared LED supply so it cannot feed the unpowered first pixel. */
    esp_err_t rc=s_led?led_strip_clear(s_led):ESP_OK;
    if(rc==ESP_OK)esp_rom_delay_us(300);
    gpio_set_level(PIN_RGB_POWER,1);side_led_off();
    s_led_powered=false;
    xSemaphoreGive(s_led_lock);return rc;
}
void board_power_off(void)
{
    board_knob_power(false);
    /* Same physical latch release order as the original bsp_power_off(). */
    gpio_set_level(PIN_KNOB_POWER,1);
    gpio_set_level(PIN_POWER,0);
}

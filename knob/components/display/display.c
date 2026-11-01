/**
 * @file display.c
 * @brief GC9A01 圆屏 + LVGL 显示栈
 *
 * SPI3 驱动 240x240 面板；部分刷新双缓冲（优先 PSRAM DMA）。
 * display_lock/unlock 供 UI 任务与 LVGL 互斥访问。
 */

#include "display.h"

#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <assert.h>
#include <sys/param.h>

static const char *TAG = "display";

/* 部分刷新行数：全帧 240 行在 BLE 启动后易耗尽内部 DMA 堆 */
#define LCD_DRAW_LINES_PREFERRED 80
#define LCD_DRAW_LINES_FALLBACK  64

static SemaphoreHandle_t s_lvgl_lock;
static lv_disp_drv_t s_disp_drv;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_t *s_display;
static esp_lcd_panel_handle_t s_panel;
static esp_timer_handle_t s_tick_timer;
static void *s_draw_buf1;
static void *s_draw_buf2;

static void *display_alloc_draw_buf(size_t bytes)
{
    void *buf = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    return buf;
}

static bool display_try_draw_bufs(uint32_t lines, bool dual, size_t *draw_buf_sz_out)
{
    size_t draw_buf_sz = BOARD_LCD_WIDTH * lines * sizeof(lv_color_t);

    if (s_draw_buf1 != NULL) {
        heap_caps_free(s_draw_buf1);
        s_draw_buf1 = NULL;
    }
    if (s_draw_buf2 != NULL) {
        heap_caps_free(s_draw_buf2);
        s_draw_buf2 = NULL;
    }

    s_draw_buf1 = display_alloc_draw_buf(draw_buf_sz);
    if (s_draw_buf1 == NULL) {
        return false;
    }

    s_draw_buf2 = NULL;
    if (dual) {
        s_draw_buf2 = display_alloc_draw_buf(draw_buf_sz);
        if (s_draw_buf2 == NULL) {
            heap_caps_free(s_draw_buf1);
            s_draw_buf1 = NULL;
            return false;
        }
    }

    *draw_buf_sz_out = draw_buf_sz;
    return true;
}

static bool display_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_drv);
    return false;
}

static void display_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)disp_drv->user_data;
    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    uint32_t px_count = (uint32_t)w * (uint32_t)h;

#if LV_COLOR_16_SWAP
    /* Kconfig swap enabled — pixels already in panel byte order */
#else
    lv_color_t *c = px_map;
    for (uint32_t i = 0; i < px_count; i++) {
        c[i].full = (uint16_t)((c[i].full >> 8) | (c[i].full << 8));
    }
#endif

    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel draw_bitmap failed: %s — unblocking LVGL", esp_err_to_name(err));
        lv_disp_flush_ready(disp_drv);
    }
}

static void display_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

esp_err_t display_init(void)
{
    s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lvgl_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t draw_lines = LCD_DRAW_LINES_PREFERRED;
    bool dual_buf = true;
    size_t draw_buf_sz = 0;

    if (!display_try_draw_bufs(draw_lines, dual_buf, &draw_buf_sz)) {
        draw_lines = LCD_DRAW_LINES_FALLBACK;
        dual_buf = true;
        if (!display_try_draw_bufs(draw_lines, dual_buf, &draw_buf_sz)) {
            dual_buf = false;
            if (!display_try_draw_bufs(draw_lines, dual_buf, &draw_buf_sz)) {
                ESP_LOGE(TAG, "LCD draw buffer alloc failed");
                return ESP_ERR_NO_MEM;
            }
            ESP_LOGW(TAG, "LCD draw buf fallback: %u lines, single buffer", (unsigned)draw_lines);
        } else {
            ESP_LOGW(TAG, "LCD draw buf fallback: %u lines, dual buffer", (unsigned)draw_lines);
        }
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)draw_buf_sz,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = -1,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                             &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    lv_init();

    lv_disp_draw_buf_init(&s_draw_buf, s_draw_buf1, s_draw_buf2, BOARD_LCD_WIDTH * draw_lines);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BOARD_LCD_WIDTH;
    s_disp_drv.ver_res = BOARD_LCD_HEIGHT;
    s_disp_drv.flush_cb = display_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;
    s_display = lv_disp_drv_register(&s_disp_drv);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = display_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, &s_disp_drv));

    const esp_timer_create_args_t tick_args = {
        .callback = &display_tick_cb,
        .name = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, 2000));

    ESP_LOGI(TAG, "GC9A01 %dx%d, partial buf %u lines x %s (%u bytes each)",
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, (unsigned)draw_lines,
             s_draw_buf2 != NULL ? "2" : "1", (unsigned)draw_buf_sz);
    return ESP_OK;
}

void display_lock(void)
{
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
}

void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_lock);
}

lv_disp_t *display_get_lv_disp(void)
{
    return s_display;
}

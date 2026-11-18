#include "sdkconfig.h"

#if CONFIG_BRIDGE_DISPLAY_COMPACT

#include "board_display.h"

#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "board.h"

#define TAG "board_lcd"
#define LCD_SPI_CLOCK_HZ (60 * 1000 * 1000)
#define LCD_BUFFER_LINES 20

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[32];
} jd9853_reg_t;

// JD9853 is MIPI DCS compatible, so the ST7789 driver handles reset, sleep-out,
// pixel format and drawing. These are the panel's power, timing and gamma
// registers from the Waveshare reference init, behind the DFh unlock.
static const jd9853_reg_t s_jd9853_init[] = {
    {0xDF, 2, {0x98, 0x53}},
    {0xDF, 2, {0x98, 0x53}},
    {0xB2, 1, {0x23}},
    {0xB7, 4, {0x00, 0x47, 0x00, 0x6F}},
    {0xBB, 6, {0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0}},
    {0xC0, 2, {0x44, 0xA4}},
    {0xC1, 1, {0x16}},
    {0xC3, 8, {0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77}},
    {0xC4, 12, {0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82}},
    {0xC8, 32, {0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00,
                0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00}},
    {0xD0, 5, {0x04, 0x06, 0x6B, 0x0F, 0x00}},
    {0xD7, 2, {0x00, 0x30}},
    {0xE6, 1, {0x14}},
    {0xDE, 1, {0x01}},
    {0xB7, 5, {0x03, 0x13, 0xEF, 0x35, 0x35}},
    {0xC1, 3, {0x14, 0x15, 0xC0}},
    {0xC2, 2, {0x06, 0x3A}},
    {0xC4, 2, {0x72, 0x12}},
    {0xBE, 1, {0x00}},
    {0xDE, 1, {0x02}},
    {0xE5, 3, {0x00, 0x02, 0x00}},
    {0xE5, 3, {0x01, 0x02, 0x00}},
    {0xDE, 1, {0x00}},
    {0xDE, 1, {0x02}},
    {0xE5, 3, {0x00, 0x02, 0x00}},
    {0xDE, 1, {0x00}},
};

static esp_err_t jd9853_send_init(esp_lcd_panel_io_handle_t io)
{
    for (size_t i = 0; i < sizeof(s_jd9853_init) / sizeof(s_jd9853_init[0]); i++) {
        esp_err_t err = esp_lcd_panel_io_tx_param(io, s_jd9853_init[i].cmd,
                                                  s_jd9853_init[i].data,
                                                  s_jd9853_init[i].len);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static lv_display_t *s_display;

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}

esp_err_t bsp_display_backlight_on(void)
{
    esp_err_t err = gpio_set_direction(BSP_LCD_BACKLIGHT, GPIO_MODE_OUTPUT);
    if (err == ESP_OK) {
        err = gpio_set_level(BSP_LCD_BACKLIGHT, BSP_LCD_BACKLIGHT_ON);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d backlight enable failed: %s", BSP_LCD_BACKLIGHT,
                 esp_err_to_name(err));
    }
    return err;
}

lv_display_t *bsp_display_start(void)
{
    if (s_display) {
        return s_display;
    }

    const gpio_config_t backlight_gpio = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&backlight_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD backlight GPIO setup failed: %s", esp_err_to_name(err));
        return NULL;
    }
    gpio_set_level(BSP_LCD_BACKLIGHT, !BSP_LCD_BACKLIGHT_ON);

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_H_RES * LCD_BUFFER_LINES * sizeof(lv_color_t),
    };
    err = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD SPI bus setup failed: %s", esp_err_to_name(err));
        return NULL;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_SPI_CS,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD SPI panel IO setup failed: %s", esp_err_to_name(err));
        return NULL;
    }

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel_handle);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel_handle);
    }
    if (err == ESP_OK) {
        err = jd9853_send_init(io_handle);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(panel_handle, BSP_LCD_Y_OFFSET, BSP_LCD_X_OFFSET);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(panel_handle, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_handle, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JD9853 initialization failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port initialization failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = BSP_LCD_H_RES * LCD_BUFFER_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_V_RES,
        .vres = BSP_LCD_H_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    s_display = lvgl_port_add_disp(&display_config);
    if (!s_display) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return NULL;
    }

    ESP_LOGI(TAG, "JD9853 172x320 display ready");
    return s_display;
}

#endif

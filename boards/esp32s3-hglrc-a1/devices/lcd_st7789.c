#include "sdkconfig.h"

#if CONFIG_BRIDGE_HGLRC_DISPLAY

#include "lcd_st7789.h"

#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "board.h"

#define TAG "hglrc_lcd"
#define LCD_SPI_CLOCK_HZ (60 * 1000 * 1000)
#define LCD_BUFFER_LINES 40

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
        err = gpio_set_level(BSP_LCD_BACKLIGHT, 1);
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
    gpio_set_level(BSP_LCD_BACKLIGHT, 0);

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
        .spi_mode = 3,
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
        err = esp_lcd_panel_set_gap(panel_handle, BSP_LCD_Y_OFFSET, BSP_LCD_X_OFFSET);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(panel_handle, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel_handle, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 initialization failed: %s", esp_err_to_name(err));
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

    ESP_LOGI(TAG, "ST7789 172x320 display ready");
    return s_display;
}

#endif

#include "sdkconfig.h"

#if CONFIG_BRIDGE_DISPLAY_TOUCH

#include "board_display.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

#define TAG "board_lcd"

// Draw buffers must be internal DMA RAM: from PSRAM the SPI driver either
// bounce-buffers every flush through internal RAM or, reading PSRAM directly,
// underruns the QSPI bus while code also executes from PSRAM.
#define LCD_BUFFER_LINES 16
#define LCD_BRIGHTNESS_CMD 0x51
#define I2C_HZ (400 * 1000)
#define I2C_TIMEOUT_MS 50

#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03

// Vendor init sequence, with brightness held at 0 until the first frame is in.
static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {LCD_BRIGHTNESS_CMD, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static lv_display_t *s_display;
static esp_lcd_panel_io_handle_t s_io;

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
    // QSPI command word: write opcode 0x02, then the DCS command.
    const uint8_t level = 0xFF;
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (LCD_BRIGHTNESS_CMD << 8),
                                              &level, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AMOLED brightness set failed: %s", esp_err_to_name(err));
    }
    return err;
}

// The panel only accepts windows that start on an even pixel and span an even
// number of them.
static void round_area_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static esp_err_t expander_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t buf[] = {reg, value};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

// Pulse the panel and touch resets on the TCA9554, leaving its other pins as
// inputs.
static esp_err_t panel_reset(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_EXPANDER_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &dev);
    if (err != ESP_OK) {
        return err;
    }
    err = expander_write(dev, TCA9554_REG_OUTPUT, 0x00);
    if (err == ESP_OK) {
        err = expander_write(dev, TCA9554_REG_CONFIG, (uint8_t)~BOARD_EXPANDER_RESET_MASK);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = expander_write(dev, TCA9554_REG_OUTPUT, BOARD_EXPANDER_RESET_MASK);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    i2c_master_bus_rm_device(dev);
    return err;
}

typedef esp_err_t (*touch_new_fn)(const esp_lcd_panel_io_handle_t io,
                                  const esp_lcd_touch_config_t *config,
                                  esp_lcd_touch_handle_t *out_touch);

typedef struct {
    const char *name;
    esp_lcd_panel_io_i2c_config_t io_config;
    touch_new_fn create;
    uint16_t x_gap;
} touch_variant_t;

// The CST816 only answers I2C for a short while after a touch, so it cannot be
// probed for. The FT3168 always answers: its absence means the CST816/CO5300
// revision.
static const touch_variant_t *touch_detect(i2c_master_bus_handle_t bus)
{
    static const touch_variant_t ft3168 = {
        "FT3168", ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG(), esp_lcd_touch_new_i2c_ft5x06, 0,
    };
    static const touch_variant_t cst816 = {
        "CST816", ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG(), esp_lcd_touch_new_i2c_cst816s, BSP_LCD_X_GAP_CO5300,
    };
    const touch_variant_t *variant =
        i2c_master_probe(bus, ft3168.io_config.dev_addr, I2C_TIMEOUT_MS) == ESP_OK ? &ft3168 : &cst816;
    ESP_LOGI(TAG, "%s touch revision", variant->name);
    return variant;
}

static lv_indev_t *touch_start(lv_display_t *display, i2c_master_bus_handle_t bus,
                               const touch_variant_t *variant)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = variant->io_config;
    io_config.scl_speed_hz = I2C_HZ;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch panel IO setup failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const esp_lcd_touch_config_t touch_config = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = BSP_TOUCH_INT,
    };
    esp_lcd_touch_handle_t touch = NULL;
    err = variant->create(io, &touch_config, &touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s initialization failed: %s", variant->name, esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_touch_cfg_t lvgl_touch = {
        .disp = display,
        .handle = touch,
    };
    return lvgl_port_add_touch(&lvgl_touch);
}

lv_display_t *bsp_display_start(void)
{
    if (s_display) {
        return s_display;
    }

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = BOARD_PMU_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c = NULL;
    esp_err_t err = i2c_new_master_bus(&i2c_config, &i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus setup failed: %s", esp_err_to_name(err));
        return NULL;
    }
    err = panel_reset(i2c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9554 panel reset failed: %s", esp_err_to_name(err));
    }
    const touch_variant_t *touch = touch_detect(i2c);

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
        BSP_LCD_H_RES * LCD_BUFFER_LINES * sizeof(uint16_t));
    err = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD QSPI bus setup failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_config, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD QSPI panel IO setup failed: %s", esp_err_to_name(err));
        return NULL;
    }

    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t panel = NULL;
    err = esp_lcd_new_panel_co5300(s_io, &panel_config, &panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_reset(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(panel, touch->x_gap, 0);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AMOLED initialization failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port initialization failed: %s", esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = s_io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * LCD_BUFFER_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
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
    lvgl_port_lock(0);
    lv_display_add_event_cb(s_display, round_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lvgl_port_unlock();

    if (!touch_start(s_display, i2c, touch)) {
        ESP_LOGW(TAG, "touch unavailable; display is read-only");
    }

    ESP_LOGI(TAG, "AMOLED %dx%d display ready", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return s_display;
}

#endif

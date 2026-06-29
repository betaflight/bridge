/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software. You can redistribute this software
 * and/or modify this software under the terms of the GNU General
 * Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later
 * version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include "leds.h"
#include "board.h"
#include "wifi.h"
#include "usb_cdc_host.h"
#include "tcp_server.h"
#include "bridge.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#if defined(BOARD_WIFI_LED_GPIO)
#include "driver/gpio.h"
#endif
#if defined(BOARD_RGB_LED_GPIO)
#include "led_strip.h"
static led_strip_handle_t s_rgb;
#endif

#if defined(BOARD_WIFI_LED_GPIO) || defined(BOARD_RGB_LED_GPIO)

static const char *TAG = "leds";

#define TICK_MS  50   // LED update period

#if defined(BOARD_WIFI_LED_GPIO)
static void wifi_led_set(int on)
{
#if defined(BOARD_WIFI_LED_ACTIVE_LOW)
    gpio_set_level(BOARD_WIFI_LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(BOARD_WIFI_LED_GPIO, on ? 1 : 0);
#endif
}
#endif

#if defined(BOARD_RGB_LED_GPIO)
static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rgb) {
        return;
    }
    led_strip_set_pixel(s_rgb, 0, r, g, b);
    led_strip_refresh(s_rgb);
}
#endif

static void led_task(void *arg)
{
    uint32_t tick = 0;
#if defined(BOARD_RGB_LED_GPIO)
    uint32_t last_activity = bridge_fc_activity();
#endif

    for (;;) {
#if defined(BOARD_WIFI_LED_GPIO)
        // Blink cadence encodes WiFi state: solid = connected as station,
        // fast = associating, slow = AP setup mode / idle.
        wifi_status_t w;
        wifi_sta_status(&w);
        int on;
        if (w.state == WIFI_STA_CONNECTED) {
            on = 1;                                  // solid
        } else if (w.state == WIFI_STA_CONNECTING) {
            on = (tick / 2) & 1;                     // ~5 Hz
        } else {
            on = (tick / (500 / TICK_MS)) & 1;       // ~1 Hz
        }
        wifi_led_set(on);
#endif

#if defined(BOARD_RGB_LED_GPIO)
        // NeoPixel encodes the FC link on the OTG port:
        //   dim red   - no FC attached
        //   amber     - FC VCP open, idle
        //   green     - FC + Configurator (TCP) linked
        //   blue flash- bytes moving to/from the FC
        bool usb = usb_cdc_host_is_connected();
        uint32_t activity = bridge_fc_activity();
        bool traffic = (activity != last_activity);
        last_activity = activity;

        if (!usb) {
            rgb_set(8, 0, 0);
        } else if (traffic) {
            rgb_set(0, 0, 40);
        } else if (tcp_server_client_connected()) {
            rgb_set(0, 24, 0);
        } else {
            rgb_set(12, 7, 0);
        }
#endif

        tick++;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void leds_start(void)
{
#if defined(BOARD_WIFI_LED_GPIO)
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_WIFI_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    wifi_led_set(0);
#endif

#if defined(BOARD_RGB_LED_GPIO)
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = BOARD_RGB_LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_rgb) == ESP_OK) {
        led_strip_clear(s_rgb);
    } else {
        ESP_LOGW(TAG, "WS2812 init failed on GPIO%d", BOARD_RGB_LED_GPIO);
    }
#endif

    xTaskCreate(led_task, "leds", 3072, NULL, 3, NULL);
}

#else  // no LEDs defined for this board

void leds_start(void) { }

#endif

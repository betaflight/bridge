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

#include "usb_download.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"

#include "display.h"

#define TAG "usb_download"

#define BOOT_GPIO GPIO_NUM_0
#define POLL_MS 50
#define HOLD_MS 3000

void usb_download_enter(void)
{
    ESP_LOGW(TAG, "rebooting into the ROM loader for USB flashing");
    display_notice("USB flash mode\n\nFlash over USB now.\nPress RESET to leave.");
    vTaskDelay(pdMS_TO_TICKS(100));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void boot_button_task(void *arg)
{
    uint32_t held_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        held_ms = gpio_get_level(BOOT_GPIO) == 0 ? held_ms + POLL_MS : 0;
        if (held_ms >= HOLD_MS) {
            usb_download_enter();
        }
    }
}

void usb_download_start(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
        ESP_LOGE(TAG, "BOOT button setup failed");
        return;
    }
    BaseType_t ok = xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 2, NULL);
    configASSERT(ok == pdPASS);
}

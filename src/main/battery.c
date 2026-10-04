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

#include "battery.h"
#include "sdkconfig.h"

#if CONFIG_BRIDGE_BATTERY_AXP2101

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board.h"

static const char *TAG = "battery";

#define AXP2101_ADDR        0x34
#define AXP2101_I2C_HZ      400000
#define AXP2101_TIMEOUT_MS  50
#define POLL_MS             2000

#define REG_STATUS1         0x00   // bit 3: battery present
#define REG_STATUS2         0x01   // bits 6:5 = 01: charging
#define REG_GAUGE_CTRL      0x18   // bit 3: fuel gauge enable
#define REG_ADC_ENABLE      0x30   // bit 0: VBAT measurement enable
#define REG_VBAT_H          0x34   // VBAT in mV, 13 bits across H[4:0]:L
#define REG_VBAT_L          0x35
#define REG_BAT_PERCENT     0xA4

static i2c_master_dev_handle_t s_pmu;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static battery_status_t s_status;
static bool s_valid;

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1, AXP2101_TIMEOUT_MS);
}

static esp_err_t set_bits(uint8_t reg, uint8_t bits)
{
    uint8_t val;
    esp_err_t err = read_reg(reg, &val);
    if (err == ESP_OK && (val & bits) != bits) {
        const uint8_t buf[2] = { reg, (uint8_t)(val | bits) };
        err = i2c_master_transmit(s_pmu, buf, sizeof(buf), AXP2101_TIMEOUT_MS);
    }
    return err;
}

static esp_err_t poll(battery_status_t *out)
{
    uint8_t st1, st2, vh, vl, pct;
    esp_err_t err = read_reg(REG_STATUS1, &st1);
    if (err == ESP_OK) err = read_reg(REG_STATUS2, &st2);
    if (err == ESP_OK) err = read_reg(REG_VBAT_H, &vh);
    if (err == ESP_OK) err = read_reg(REG_VBAT_L, &vl);
    if (err == ESP_OK) err = read_reg(REG_BAT_PERCENT, &pct);
    if (err != ESP_OK) {
        return err;
    }
    out->present = st1 & 0x08;
    if (out->present && pct > 100) {
        return ESP_ERR_INVALID_RESPONSE;   // gauge not ready yet
    }
    out->charging = ((st2 >> 5) & 0x03) == 0x01;
    out->mv = ((vh & 0x1F) << 8) | vl;
    out->percent = out->present ? pct : 0;
    return ESP_OK;
}

static void battery_task(void *arg)
{
    (void)arg;
    for (;;) {
        battery_status_t st;
        esp_err_t err = poll(&st);
        // A gauge that is not ready keeps the last good reading; a PMU that
        // stops answering does not.
        if (err != ESP_ERR_INVALID_RESPONSE) {
            taskENTER_CRITICAL(&s_mux);
            s_valid = (err == ESP_OK);
            if (s_valid) {
                s_status = st;
            }
            taskEXIT_CRITICAL(&s_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void battery_start(void)
{
    i2c_master_bus_handle_t bus;
    if (i2c_master_get_bus_handle(BOARD_PMU_I2C_PORT, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "no I2C bus on port %d; battery monitor disabled", BOARD_PMU_I2C_PORT);
        return;
    }
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = AXP2101_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev, &s_pmu);
    if (err == ESP_OK) {
        err = set_bits(REG_ADC_ENABLE, 0x01);
    }
    if (err == ESP_OK) {
        err = set_bits(REG_GAUGE_CTRL, 0x08);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not responding (%s); battery monitor disabled", esp_err_to_name(err));
        return;
    }
    BaseType_t ok = xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL);
    configASSERT(ok == pdTRUE);
    ESP_LOGI(TAG, "AXP2101 battery monitor started");
}

bool battery_get(battery_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    bool valid = s_valid;
    if (valid) {
        *out = s_status;
    }
    taskEXIT_CRITICAL(&s_mux);
    return valid;
}

#else

void battery_start(void) { }

bool battery_get(battery_status_t *out)
{
    (void)out;
    return false;
}

#endif

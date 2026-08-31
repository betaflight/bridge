#include "adc_voltage.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"

#define TAG "adc_voltage"


#define ADC_SAMPLE_COUNT            16
#define ADC_SAMPLE_PERIOD_MS       1200

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;
static adc_channel_t s_adc_channel;
static adc_unit_t s_adc_unit;
static volatile uint32_t s_voltage_mv;

static void adc_voltage_task(void *arg)
{
    (void)arg;

    while (1) {
        int raw_sum = 0;
        int sample_count = 0;

        for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
            int raw = 0;
            if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) == ESP_OK) {
                raw_sum += raw;
                sample_count++;
            }
        }

        if (sample_count > 0) {
            int adc_mv = 0;
            if (adc_cali_raw_to_voltage(s_adc_cali_handle,
                                        raw_sum / sample_count, &adc_mv) == ESP_OK) {
                s_voltage_mv = (uint32_t)adc_mv * BOARD_ADC_DIVIDER_RATIO;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_PERIOD_MS));
    }
}

esp_err_t adc_voltage_init(void)
{
    esp_err_t err = adc_oneshot_io_to_channel(
        BOARD_ADC_GPIO, &s_adc_unit, &s_adc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC input: %s",
                 BOARD_ADC_GPIO, esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_adc_unit,
        .chan = s_adc_channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC calibration init failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t task_ok = xTaskCreate(adc_voltage_task, "adc_voltage", 3072,
                                     NULL, 3, NULL);
    if (task_ok != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GPIO%d -> ADC%d channel %d, averaging %d samples",
             BOARD_ADC_GPIO, s_adc_unit + 1, s_adc_channel, ADC_SAMPLE_COUNT);
    return ESP_OK;
}

uint32_t adc_voltage_get_mv(void)
{
    return s_voltage_mv;
}

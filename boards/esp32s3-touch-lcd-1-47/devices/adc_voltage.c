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

typedef struct {
    adc_channel_t channel;
    adc_cali_handle_t cali;
} adc_input_t;

static const gpio_num_t s_gpios[] = BOARD_ADC_GPIOS;
#define ADC_INPUT_COUNT (sizeof(s_gpios) / sizeof(s_gpios[0]))

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_unit_t s_adc_unit;
static adc_input_t s_inputs[ADC_INPUT_COUNT];
static volatile uint32_t s_voltage_mv;

static uint32_t read_input_mv(const adc_input_t *input)
{
    int raw_sum = 0;
    int sample_count = 0;

    for (int i = 0; i < ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, input->channel, &raw) == ESP_OK) {
            raw_sum += raw;
            sample_count++;
        }
    }

    int adc_mv = 0;
    if (sample_count == 0 ||
        adc_cali_raw_to_voltage(input->cali, raw_sum / sample_count, &adc_mv) != ESP_OK) {
        return 0;
    }
    return (uint32_t)adc_mv * BOARD_ADC_DIVIDER_RATIO;
}

static void adc_voltage_task(void *arg)
{
    (void)arg;

    while (1) {
        uint32_t highest_mv = 0;
        for (size_t i = 0; i < ADC_INPUT_COUNT; i++) {
            uint32_t mv = read_input_mv(&s_inputs[i]);
            if (mv > highest_mv) {
                highest_mv = mv;
            }
        }
        s_voltage_mv = highest_mv;

        vTaskDelay(pdMS_TO_TICKS(ADC_SAMPLE_PERIOD_MS));
    }
}

static esp_err_t input_init(adc_input_t *input, gpio_num_t gpio)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit, &input->channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC input: %s", gpio, esp_err_to_name(err));
        return err;
    }

    if (!s_adc_handle) {
        const adc_oneshot_unit_init_cfg_t unit_cfg = {
            .unit_id = unit,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_adc_unit = unit;
    } else if (unit != s_adc_unit) {
        ESP_LOGE(TAG, "GPIO%d is on ADC%d, not ADC%d", gpio, unit + 1, s_adc_unit + 1);
        return ESP_ERR_INVALID_ARG;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, input->channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit,
        .chan = input->channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &input->cali);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC calibration init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "GPIO%d -> ADC%d channel %d", gpio, unit + 1, input->channel);
    return ESP_OK;
}

esp_err_t adc_voltage_init(void)
{
    for (size_t i = 0; i < ADC_INPUT_COUNT; i++) {
        esp_err_t err = input_init(&s_inputs[i], s_gpios[i]);
        if (err != ESP_OK) {
            return err;
        }
    }

    BaseType_t task_ok = xTaskCreate(adc_voltage_task, "adc_voltage", 3072,
                                     NULL, 3, NULL);
    if (task_ok != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint32_t adc_voltage_get_mv(void)
{
    return s_voltage_mv;
}

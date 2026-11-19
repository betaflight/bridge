#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t adc_voltage_init(void);
uint32_t adc_voltage_get_mv(void);

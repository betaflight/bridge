#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

lv_display_t *bsp_display_start(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
esp_err_t bsp_display_backlight_on(void);

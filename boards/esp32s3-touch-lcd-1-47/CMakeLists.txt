# Board component for esp32s3-touch-lcd-1-47: publishes board.h/board_display.h,
# the directly driven panel and the battery monitor to whoever requires this board.

idf_component_register(
    SRCS
        "devices/lcd_jd9853.c"
        "devices/adc_voltage.c"
        "assets/fonts/ui_font_size14.c"
        "assets/fonts/ui_font_size24.c"
    INCLUDE_DIRS
        "."
        "devices"
        "assets/img"
    REQUIRES
        esp_lcd
        esp_adc
        esp_driver_gpio
        esp_driver_spi
)

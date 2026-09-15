# Board component for esp32s3-hglrc-a1: publishes board.h/board_display.h, the
# directly driven panel and the voltage monitor to whoever requires this board.

idf_component_register(
    SRCS
        "devices/lcd_st7789.c"
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

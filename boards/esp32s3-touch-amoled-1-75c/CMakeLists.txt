# Board component for esp32s3-touch-amoled-1-75c: publishes board.h/board_display.h
# and the directly driven AMOLED + touch to whoever requires this board.

idf_component_register(
    SRCS
        "devices/lcd_co5300.c"
        "devices/lv_mem_psram.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_lcd
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
    # lv_mem_psram.c only defines symbols LVGL calls, so the linker would
    # otherwise never pull it out of the archive.
    WHOLE_ARCHIVE
)

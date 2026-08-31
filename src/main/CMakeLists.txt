set(hglrc_display_requires)
set(hglrc_display_sources)
set(hglrc_display_include_dirs)
if(CONFIG_BRIDGE_HGLRC_DISPLAY)
    list(APPEND hglrc_display_requires esp_lcd esp_lvgl_port)
    list(APPEND hglrc_display_sources
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/devices/adc_voltage.c"
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/devices/lcd_st7789.c"
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/assets/fonts/ui_font_size14.c"
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/assets/fonts/ui_font_size24.c")
    list(APPEND hglrc_display_include_dirs
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/devices"
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}/assets/img")
endif()

idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "tcp_server.c"
        "usb_cdc_host.c"
        "bridge.c"
        "http_status.c"
        "ws_serial.c"
        "tls_cert.c"
        "ota.c"
        "leds.c"
        "display.c"
        "bridge_mdns.c"
        ${hglrc_display_sources}
    EMBED_TXTFILES
        "icon.svg"                              # favicon served at /icon.svg
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}"   # board.h for the selected board
        ${hglrc_display_include_dirs}
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_adc
        usb
        lwip
        esp_http_server
        esp_https_server
        mbedtls
        app_update
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        espressif__mdns
)

# The release workflow stamps the firmware version in with
# `idf.py -DBRIDGE_VERSION=<tag>`; otherwise the default in http_status.c is used.
if(DEFINED BRIDGE_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BRIDGE_VERSION=\"${BRIDGE_VERSION}\"")
endif()

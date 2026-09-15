idf_build_get_property(board BRIDGE_BOARD)

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
    EMBED_TXTFILES
        "icon.svg"                              # favicon served at /icon.svg
    INCLUDE_DIRS
        "."
    REQUIRES
        "${board}"                              # board.h, board_display.h, board peripherals
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        usb
        lwip
        esp_http_server
        esp_https_server
        mbedtls
        app_update
        esp_app_format
        esp_timer
        esp_driver_gpio
        esp_driver_i2c
        espressif__mdns
)

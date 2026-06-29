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
    EMBED_TXTFILES
        "icon.svg"                              # favicon served at /icon.svg
    INCLUDE_DIRS
        "."
        "${CMAKE_SOURCE_DIR}/boards/${BOARD}"   # board.h for the selected board
    REQUIRES
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
        esp_driver_gpio
)

# The release workflow stamps the firmware version in with
# `idf.py -DBRIDGE_VERSION=<tag>`; otherwise the default in http_status.c is used.
if(DEFINED BRIDGE_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BRIDGE_VERSION=\"${BRIDGE_VERSION}\"")
endif()

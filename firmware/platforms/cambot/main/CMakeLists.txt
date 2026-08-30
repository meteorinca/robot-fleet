# main/CMakeLists.txt

set(SRCS
    "main.c"
    "camera.c"
    "led.c"
    "motor.c"
    "wifi_mgr.c"
    "webserver.c"
    "ota_mgr.c"
    "timekeep.c"
)

idf_component_register(
    SRCS            ${SRCS}
    INCLUDE_DIRS    "."
                    "${CMAKE_SOURCE_DIR}/boards/${BOARD}"   # makes board_config.h includable directly
    PRIV_REQUIRES   esp_driver_gpio
                    esp_driver_ledc
                    esp_wifi
                    esp_event
                    esp_netif
                    esp_timer
                    nvs_flash
                    esp_http_server
                    mdns
                    esp_app_format
                    app_update
                    espressif__esp32-camera
)

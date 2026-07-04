# main/CMakeLists.txt  —  CarBot
#
# Source files common to ALL boards
set(COMMON_SRCS
    "main.c"
    "motor.c"
    "wifi_mgr.c"
    "webserver.c"
    "ota_mgr.c"
    "timekeep.c"
    "ultrasonic.c"
    "oled.c"
    "buzzer.c"
)

list(APPEND CONDITIONAL_SRCS "led.c")
message(STATUS "━━━ [main] LED driver: led.c (simple GPIO/LEDC) ━━━")

# Boards that include a 433 MHz RF module
set(HAS_RF_BOARDS "")

if(BOARD IN_LIST HAS_RF_BOARDS)
    list(APPEND CONDITIONAL_SRCS "rf.c")
    message(STATUS "  [main] RF module: ENABLED (RCSwitch)")
else()
    message(STATUS "  [main] RF module: DISABLED (board=${BOARD})")
endif()

# ── Component registration ────────────────────────────────────────────────────
idf_component_register(
    SRCS            ${COMMON_SRCS} ${CONDITIONAL_SRCS}
    INCLUDE_DIRS    "."
                    "${CMAKE_SOURCE_DIR}/boards/${BOARD}"   # makes board_config.h includable directly
    EMBED_TXTFILES  "index.html"
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
                    esp_driver_i2c
                    driver
)
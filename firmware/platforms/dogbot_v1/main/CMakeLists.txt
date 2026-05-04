# main/CMakeLists.txt
#
# Source files common to ALL boards
set(COMMON_SRCS
    "main.c"
    "servo.c"
    "dog_actions.c"
    "wifi_mgr.c"
    "timekeep.c"
    "webserver.c"
    "ota_mgr.c"
    "touch_input.c"   # safe on all chips — SOC guard inside makes it a no-op on C3
)

# Boards that use a WS2812/NeoPixel strip instead of a simple GPIO LED
set(WS2812_BOARDS "")

# Boards that have the OLED display and audio peripherals (dog_peripherals.c)
set(DISPLAY_BOARDS "esp32c3_dog")

if(BOARD IN_LIST WS2812_BOARDS)
    list(APPEND CONDITIONAL_SRCS "ws2812.c")
    message(STATUS "━━━ [main] LED driver: ws2812.c (NeoPixel RMT) ━━━")
else()
    list(APPEND CONDITIONAL_SRCS "led.c")
    message(STATUS "━━━ [main] LED driver: led.c (simple GPIO/LEDC) ━━━")
endif()

if(BOARD IN_LIST DISPLAY_BOARDS)
    list(APPEND CONDITIONAL_SRCS "dog_peripherals.c")
    message(STATUS "━━━ [main] Display & Audio peripherals: ENABLED ━━━")
endif()


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
    REQUIRES        RCSwitch
                    led_strip
                    esp_driver_rmt
                    esp_driver_touch_sens
                    esp_lcd
    PRIV_REQUIRES   esp_driver_gpio
                    esp_driver_ledc
                    esp_wifi
                    esp_event
                    esp_netif
                    esp_timer
                    nvs_flash
                    esp_http_server
                    mdns
                    driver
                    esp_app_format
                    app_update
                    esp_adc
)
# main/CMakeLists.txt
#
# Source files common to ALL boards
set(COMMON_SRCS
    "main.c"
    "servo.c"
    "dog_actions.c"
    "wifi_mgr.c"
    "webserver.c"
    "ota_mgr.c"
    "timekeep.c"
    "oled.c"
    "touch_input.c"
)

# Boards that include a 433 MHz RF module
set(HAS_RF_BOARDS "")

if(BOARD IN_LIST HAS_RF_BOARDS)
    list(APPEND CONDITIONAL_SRCS "rf.c")
    message(STATUS "━━━ [main] RF module: ENABLED (RCSwitch) ━━━")
else()
    message(STATUS "━━━ [main] RF module: DISABLED (board=${BOARD}) ━━━")
endif()


list(APPEND CONDITIONAL_SRCS "led.c")
message(STATUS "━━━ [main] LED driver: led.c (simple GPIO/LEDC) ━━━")

# AUDIO_AMP_GPIO is defined in board_config.h — dog_peripherals.c handles I2S PDM speaker
list(APPEND CONDITIONAL_SRCS "dog_peripherals.c")
message(STATUS "━━━ [main] Audio (I2S PDM Speaker) peripherals: ENABLED ━━━")

if(ENABLE_ULTRASONIC)
    list(APPEND CONDITIONAL_SRCS "ultrasonic.c")
    message(STATUS "━━━ [main] Ultrasonic Sensor: ENABLED ━━━")
else()
    message(STATUS "━━━ [main] Ultrasonic Sensor: DISABLED ━━━")
endif()


# ── Component registration ────────────────────────────────────────────────────
idf_component_register(
    SRCS            ${COMMON_SRCS} ${CONDITIONAL_SRCS}
    INCLUDE_DIRS    "."
                    "${CMAKE_SOURCE_DIR}/boards/${BOARD}"   # makes board_config.h includable directly
    EMBED_TXTFILES  "index.html"
    REQUIRES        led_strip
                    esp_driver_rmt
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

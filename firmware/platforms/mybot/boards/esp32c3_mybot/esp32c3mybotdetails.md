
# ESP32-C3 MyBot Super Mini


The ESP32-C3 Super Mini is an ultra-compact development board featuring 16 main header pins (8 on each side) with a standard 2.54 mm spacing. It breaks out 13 General Purpose Input/Output (GPIO) pins, power inputs, and dedicated communication buses.

## Complete Header Pinout Map

The board is laid out symmetrically with the USB-C port facing "up."

| Left Side Pins (Top to Bottom) | Function / Alternative Uses | Right Side Pins (Top to Bottom) | Function / Alternative Uses |
|---|---|---|---|
| GND | Ground | 5V | 5V Power Input / Output |
| GPIO 0 | ADC1_CH0, PWM | 3V3 | 3.3V Power Output / Input |
| GPIO 1 | ADC1_CH1, PWM | GPIO 21 | UART TX |
| GPIO 2 | ADC1_CH2, Boot Strapping Pin | GPIO 20 | UART RX |
| GPIO 3 | ADC1_CH3, PWM | GPIO 10 | PWM |
| GPIO 4 | ADC1_CH4, SPI SCK, PWM | GPIO 9 | Onboard BOOT Button, I2C SCL |
| GPIO 5 | ADC2_CH0, SPI MISO, PWM | GPIO 8 | Onboard Blue LED, I2C SDA |
| GPIO 6 | SPI MOSI, PWM | GPIO 7 | SPI SS, PWM |

## MyBot Connection Guide
| Components | Connection Pin | Note |
|---|---|---|
| Ultrasonic Sensor (HC-SR04) | VCC: 5V | | |
| | Trigger: GPIO 10 | | |
| | Echo: GPIO 4 | | |
| | GND: GND | | |
| Continuous Rotation Servo | VCC: 5V | | |
| | Signal: GPIO 5 | | |
| | GND: GND | | |
| Neopixel Strip | VCC: 5V | |
| | Data: GPIO 3 | |
| | GND: GND | |
| 0.96" OLED (I2C) | VCC: 3.3V or 5V | |
| | SDA: GPIO 6 | |
| | SCL: GPIO 7 | |
| | GND: GND | |
| Buttons | Pin | Note |
| | User Button 1: GPIO 0 | | |
| | User Button 2: GPIO 1 | | |
------------------------------
## Key Feature Configurations

* Onboard LED: The user-controllable blue status LED is connected to GPIO 8. Note that it operates on inverted logic (LOW turns the LED on).
* I2C Bus: We use GPIO 6 (SDA) and GPIO 7 (SCL) for the OLED. (Default hardware is 8/9, but 8 conflicts with the built-in LED).
* SPI Bus (SPI2): Uses GPIO 4 (SCK), GPIO 5 (MISO), GPIO 6 (MOSI), and GPIO 7 (CS/SS). *Note: SPI MOSI/CS overlap with our custom I2C pins, so SPI cannot be used simultaneously with the OLED.*
* Analog Inputs: GPIO 0 through 4 belong to the highly accurate, factory-calibrated ADC1. GPIO 5 maps to ADC2, but you should avoid using it for analog readings if Wi-Fi is turned on due to internal chip limitations. [1, 2, 4, 6, 7, 8] 

------------------------------
## Important Usage Tips

* Logic Levels: The board operates strictly on 3.3V logic. Connecting 5V components directly to the GPIO pins will permanently damage the microcontroller. [1, 9] 
* Dual Power Sources: You can power the board through the 5V pin (accepts 3.3V to 6V) or the USB-C port. Never connect an external power supply to the 5V pin while the USB-C cable is simultaneously plugged into your computer. [9] 
* Boot Strapping Pins: GPIO 2, 8, and 9 dictate how the chip boots up during reset. Avoid attaching sensors or pull-down resistors to these pins that might alter their state at startup, or your code may fail to flash or execute properly.
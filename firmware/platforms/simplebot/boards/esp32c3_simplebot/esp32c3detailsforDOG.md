**4 Servo Motors Connections:**
| Servo | GPIO Pin | Description |
|-------|----------|-------------|
| **FL_GPIO_NUM** (Front Left) | **GPIO 21** | Front left leg servo |
| **FR_GPIO_NUM** (Front Right) | **GPIO 19** | Front right leg servo |
| **BL_GPIO_NUM** (Back Left) | **GPIO 20** | Back left leg servo |
| **BR_GPIO_NUM** (Back Right) | **GPIO 18** | Back right leg servo |

**OLED Display Connections:**
| Display Signal | GPIO Pin | Notes |
|----------------|----------|-------|
| **MOSI** (Data) | **GPIO 4** | SPI MOSI data line |
| **CLK** (Clock) | **GPIO 5** | SPI clock line |
| **DC** (Data/Command) | **GPIO 10** | Data/Command select |
| **RST** (Reset) | **GPIO_NC** | Not connected (NC = No Connect) |
| **CS** (Chip Select) | **GPIO_NC** | Not connected (likely tied to GND or not used) |

1. **Display Type**: Your board uses an **ILI9341 LCD** (based on the code), but the config shows `LCD_TYPE_ST7789_SERIAL` - this might be a configuration inconsistency.

2. **Display Resolution**: 160x80 pixels (small display)

3. **Servo Control**: The servos are controlled via the `servo_dog_ctrl` library which likely uses PWM signals on these GPIO pins.

4. **Other GPIOs in use**:
   - GPIO 6, 7: Audio speaker (PDM)
   - GPIO 3: Audio amplifier control
   - GPIO 9: Boot button
   - GPIO 0: Move wake button
   - GPIO 1: Audio wake button
   - GPIO 8: LED strip (from the main code)


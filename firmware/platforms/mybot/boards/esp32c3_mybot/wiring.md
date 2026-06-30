## Part 1: Controller Breadboard (5-wire output)

**Components needed:** 2 LEDs, 2 resistors (220Ω-330Ω), 2 push buttons, 1 breadboard

1. **Orange wire (BTN1):** Connect Orange wire to right side of Left-button (BTN1)
2. **Yellow wire (LED_GRN+):** Connect from green LED anode (long leg) through resistor → to Yellow wire
3. **Purple wire (LED_RED+):** Connect from red LED anode (long leg) through resistor → to Purple wire
4. **Blue wire (BTN2):** Connect Blue wire to left side of Right-button (BTN2)
5. **Green wire (GND):** Ensure non-used side of buttons and the shorter legs of LEDs are connected together and to the Green Wire

Connect wires

---

## Part 2: Full System Wiring (ESP32-S3)

### Ultrasonic Sensor (HC-SR04)
- **VCC** → ESP32-S3 **5V** pin
- **Trig** → ESP32-S3 **GPIO 10**
- **Echo** → ESP32-S3 **GPIO 4**
- **GND** → ESP32-S3 **GND**

### Continuous Rotation Servo
- **VCC** → ESP32-S3 **5V** pin
- **Signal** → ESP32-S3 **GPIO 5**
- **GND** → ESP32-S3 **GND**

### Neopixel Strip
- **VCC** → ESP32-S3 **5V** pin
- **Data** → ESP32-S3 **GPIO 3**
- **GND** → ESP32-S3 **GND**

### 0.96" OLED (I2C)
- **VCC** → ESP32-S3 **3.3V** pin
- **SDA** → ESP32-S3 **GPIO 7**
- **SCL** → ESP32-S3 **GPIO 6**
- **GND** → ESP32-S3 **GND**

### Controller (from Part 1)
- **Orange (BTN1)** → **GPIO 0**
- **Yellow (LED_GRN+)** → **GPIO 20**
- **Purple (LED_RED+)** → **GPIO 21**
- **Blue (BTN2)** → **GPIO 1**
- **Green (GND)** → ESP32-S3 **GND**

---

## Pin Summary
| GPIO | Component |
|------|-----------|
| 0 | Button 1 |
| 1 | Button 2 |
| 3 | Neopixel Data |
| 4 | Ultrasonic Echo |
| 5 | Servo Signal |
| 6 | OLED SCL |
| 7 | OLED SDA |
| 10 | Ultrasonic Trigger |
| 20 | Green LED |
| 21 | Red LED |
| 10 | Ultrasonic Trigger |
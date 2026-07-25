# Building and Flashing Guide

This guide covers the commands used to compile, flash, and monitor the ESP-IDF firmware for your boards.

## 1. First-Time Setup & Setting the Target

When setting up a new chip for the first time (like the ESP32-C3) or switching chips, you must specify the target architecture. This command will set the target and run the initial build:

```powershell
idf.py set-target esp32c3 build
```

## 2. Selecting a Specific Board

The build system relies on the `BOARD` variable to choose the correct hardware configuration from the `boards/` directory:
- `esp32c3_rfbot` — 433 MHz learner/sender (TX: GPIO 3, RX: GPIO 10)
- `esp32c3_rfbot315` — 315 MHz learner/sender (TX: GPIO 3, RX: GPIO 10)

If you don't provide a board, it defaults to `esp32c3_rfbot`.

To change to a different board (e.g. 315 MHz), use the `-DBOARD=` argument and make sure to do a `fullclean` so old configuration files are cleared out:

```powershell
idf.py -DBOARD=esp32c3_rfbot315 set-target esp32c3 fullclean build
```

> **Note:** The `BOARD` variable is cached! After running this command once, subsequent builds for the same board only require:
> ```powershell
> idf.py build
> ```

## 3. Setting a Device Number (Multiple Robots)

If you are running multiple robots of the same board type, you can pass a `DEVICE_NUMBER` argument to give the robot a unique identifier in the firmware:

```powershell
idf.py -D DEVICE_NUMBER=1 build
```

## 4. Flashing the Board (USB Upload)

To upload the firmware for the first time or via a direct cable connection, you need to specify the COM port the board is connected to (e.g., `COM3`, `COM4`). You can find this in the Windows Device Manager under "Ports (COM & LPT)".

Use `flash` to upload and `monitor` to immediately open the serial console:

```powershell
idf.py -p COM3 flash monitor
```
*(Replace `COM3` with your actual port).*

To exit the serial monitor, press `Ctrl + ]`.

## 5. All-In-One Commands

Here are some handy combinations you can copy and paste:

**Clean, Build for a specific board, Flash, and Monitor:**
```powershell
idf.py -D BOARD=esp32c3_rfbot fullclean build flash monitor -p COM3
```

**Build with a Device Number and Flash:**
```powershell
idf.py -D DEVICE_NUMBER=2 build flash monitor -p COM3
```

## 6. Over-The-Air (OTA) Updates

Once the firmware has been flashed via USB for the first time, you can push future updates over Wi-Fi. You can find the compiled binary at:
`build/rfbot.bin`

Use your OTA scripts (or `curl` commands) to upload this `.bin` file to the robot's IP address.

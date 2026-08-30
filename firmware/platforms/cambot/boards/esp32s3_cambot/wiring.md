# XIAO ESP32S3 Sense — CamBot Wiring Reference

**Board**: Seeed Studio XIAO ESP32S3 Sense  
**Chip**: ESP32-S3  
**Flash**: 8MB  
**PSRAM**: 8MB OPI (required for camera frame buffers)

## Camera (OV2640 — Sense Expansion Board B2B Connector)

Pins verified against Seeed Studio XIAO ESP32S3 Sense schematic rev 1.1:
https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/

| Signal  | GPIO | Notes                        |
|---------|------|------------------------------|
| XCLK    | 10   | Master clock to camera        |
| PCLK    | 13   | Pixel clock from camera       |
| VSYNC   | 38   | Vertical sync                 |
| HREF    | 47   | Horizontal reference          |
| SIOD    | 40   | SCCB/I2C SDA                  |
| SIOC    | 39   | SCCB/I2C SCL                  |
| D0 (Y2) | 15   | Data bit 0                    |
| D1 (Y3) | 17   | Data bit 1                    |
| D2 (Y4) | 18   | Data bit 2                    |
| D3 (Y5) | 16   | Data bit 3                    |
| D4 (Y6) | 14   | Data bit 4                    |
| D5 (Y7) | 12   | Data bit 5                    |
| D6 (Y8) | 11   | Data bit 6                    |
| D7 (Y9) | 48   | Data bit 7                    |
| PWDN    | -1   | Hardwired on expansion board  |
| RESET   | -1   | Hardwired on expansion board  |

> **Note**: PCLK=13, VSYNC=38, HREF=47, D7=48 are commonly misquoted as 11/6/7/13 in
> secondary sources. Use the values above (from Seeed schematic) for correct operation.

## Built-in LED

| Function | GPIO | Notes           |
|----------|------|-----------------|
| User LED | 21   | Active-HIGH     |

## Boot Button

| Function    | GPIO | Notes                          |
|-------------|------|--------------------------------|
| BOOT button | 0    | Press to toggle camera stream  |

## L298N Motor Driver

Wire colour → L298N input → XIAO pad → ESP32-S3 GPIO:

| Wire Colour | L298N Pin | XIAO Pad | GPIO | Motor Channel   |
|-------------|-----------|----------|------|-----------------|
| 🟢 Green    | IN1       | D0       | 1    | Steering (A+)   |
| 🔵 Blue     | IN2       | D1       | 2    | Steering (A-)   |
| 🟣 Purple   | IN3       | D2       | 3    | Drive (B+)      |
| 🩶 Grey     | IN4       | D3       | 4    | Drive (B-)      |

**Motor output mapping:**
- `OUT1` / `OUT2` → **Steering motor** (controlled by IN1/IN2)
- `OUT3` / `OUT4` → **Drive motor** (controlled by IN3/IN4)

**L298N power notes:**
- `VMS` (motor supply): connect to your battery pack (6–12 V typical)
- `VSS` (logic 5 V): bridge to XIAO 5V pin (USB power or battery regulator)
- `ENA` / `ENB`: leave jumpered HIGH (always enabled) for bang-bang control
  - Remove jumpers and connect to PWM-capable GPIO if speed control is needed later

**GPIO selection rationale:**  
GPIO 1–4 (D0–D3) are confirmed free from the camera, SD-card, and PDM microphone
peripherals per Seeed Studio XIAO ESP32S3 Sense schematic rev 1.1.  
Reference: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/

## USB (Serial / JTAG)

The XIAO ESP32S3 uses native USB on the USB-C port. No FTDI adapter required.  
Console: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`

## Flash Command

```bash
# First-time flash (from cambot/ project root)
idf.py set-target esp32s3 build flash monitor

# Override device number (sets mDNS hostname cambot2.local)
idf.py -DDEVICE_NUMBER=2 set-target esp32s3 build flash monitor
```

## Accessing the Device

- Web UI (RC car controller): `http://cambot1.local`
- MJPEG stream: `http://cambot1.local/stream`
- Snapshot: `http://cambot1.local/snapshot`
- Motor control: `http://cambot1.local/drive?steer=50&drive=100`
- Emergency stop: `http://cambot1.local/drive?stop=1`

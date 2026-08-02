# Implementation Plan - Robot Fleet Firmware v1.9 Standardization

This plan outlines the steps to audit and bring all robot platforms in the fleet (`carbot`, `dogbot_v1`, `rfbot`, `simplebot`, `speakerbot`) up to **v1.9** based on the reference implementation in `mybot` (`#define FW_VERSION "v1.9"`).

## Summary of Findings & Inconsistencies

1. **Firmware Versions (`FW_VERSION`)**:
   - `mybot`: Reference base (`"v1.9"`)
   - `carbot`: `"v1.0"` ❌
   - `dogbot_v1`: `"1.0"` ❌
   - `rfbot`: `"0.1"` (both `esp32c3_rfbot` and `esp32c3_rfbot315`) ❌
   - `simplebot`: `"0.1"` ❌
   - `speakerbot`: `"v1.0"` ❌

2. **`index.html` Architecture**:
   - `mybot`, `carbot`, `dogbot_v1`, `speakerbot`: Have separate `main/index.html` files embedded via binary assembly.
   - `rfbot`, `simplebot`: ❌ **Lacks separate `index.html` file**. HTML is hardcoded inline within `webserver.c`, and `CMakeLists.txt` does not embed `index.html`.

3. **Reset Networking & Confirmation Flow**:
   - `mybot`, `carbot`, `speakerbot`: 7-second hold on boot button displays countdown on OLED followed by 3x click confirmation to trigger `wifi_forget_all()`.
   - `dogbot_v1`: ❌ **Missing `wifi_forget_all()` implementation** and missing boot button reset handler task in `main.c`.
   - `dogbot_v1` and `speakerbot`: ❌ **Missing `resettingwifi.md` documentation** in `main/`.

4. **Naming System & mDNS Identity**:
   - `dogbot_v1`: `MDNS_HOSTNAME` is `paulbot` while platform directory is `dogbot_v1`. `MDNS_INSTANCE` missing space `"PaulBotv"`.
   - Variations in formatting across board configs.

---

## User Review Required

> [!IMPORTANT]
> All bots will be updated to `#define FW_VERSION "v1.9"`. `rfbot` and `simplebot` will have their inline HTML extracted into separate `index.html` files to standardize build and serving mechanisms across the entire fleet.

---

## Proposed Changes

### 1. Board Configurations (`board_config.h`)

#### [MODIFY] [carbot/boards/esp32c3_carbot/board_config.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/carbot/boards/esp32c3_carbot/board_config.h)
- Update `#define FW_VERSION "v1.9"`

#### [MODIFY] [dogbot_v1/boards/esp32c3_dog/board_config.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/dogbot_v1/boards/esp32c3_dog/board_config.h)
- Update `#define FW_VERSION "v1.9"`
- Standardize `MDNS_INSTANCE` to `"PaulBot v" _STR(DEVICE_NUMBER)`

#### [MODIFY] [rfbot/boards/esp32c3_rfbot/board_config.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/boards/esp32c3_rfbot/board_config.h)
- Update `#define FW_VERSION "v1.9"`

#### [MODIFY] [rfbot/boards/esp32c3_rfbot315/board_config.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/boards/esp32c3_rfbot315/board_config.h)
- Update `#define FW_VERSION "v1.9"`

#### [MODIFY] [simplebot/boards/esp32c3_simplebot/board_config.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/simplebot/boards/esp32c3_simplebot/board_config.h)
- Update `#define FW_VERSION "v1.9"`

#### [MODIFY] [speakerbot/boards/esp32c3_speakerbot/board_config.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/speakerbot/boards/esp32c3_speakerbot/board_config.h)
- Update `#define FW_VERSION "v1.9"`

---

### 2. Standalone `index.html` & Web Server Standardization

#### [NEW] [rfbot/main/index.html](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/index.html)
- Create dedicated `index.html` file using modern v1.9 glassmorphic layout, including RF transmission/reception cards, RF outlet toggles, status clock, WiFi manager, schedule action, and OTA updater.

#### [MODIFY] [rfbot/main/CMakeLists.txt](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/CMakeLists.txt)
- Add `EMBED_TXTFILES "index.html"` to `idf_component_register`.

#### [MODIFY] [rfbot/main/webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/rfbot/main/webserver.c)
- Declare `index_html_start` and `index_html_end` external symbols.
- Simplify `root_get_handler` to serve `index.html` via binary buffer stream.

#### [NEW] [simplebot/main/index.html](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/simplebot/main/index.html)
- Create dedicated `index.html` file with v1.9 design system featuring NeoPixel control cards, servo controls, action scheduler, WiFi manager, and OTA updater.

#### [MODIFY] [simplebot/main/CMakeLists.txt](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/simplebot/main/CMakeLists.txt)
- Add `EMBED_TXTFILES "index.html"` to `idf_component_register`.

#### [MODIFY] [simplebot/main/webserver.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/simplebot/main/webserver.c)
- Declare `index_html_start` and `index_html_end` external symbols.
- Simplify `root_get_handler` to serve `index.html`.

#### [MODIFY] [carbot/main/index.html](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/carbot/main/index.html)
#### [MODIFY] [dogbot_v1/main/index.html](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/dogbot_v1/main/index.html)
#### [MODIFY] [speakerbot/main/index.html](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/speakerbot/main/index.html)
- Align titles, version badges to v1.9, and ensure WiFi reset instructions and schedule card features are consistent.

---

### 3. Reset Networking & WiFi Recovery Alignment

#### [MODIFY] [dogbot_v1/main/wifi_mgr.h](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/dogbot_v1/main/wifi_mgr.h)
#### [MODIFY] [dogbot_v1/main/wifi_mgr.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/dogbot_v1/main/wifi_mgr.c)
- Add `wifi_forget_all(void)` implementation to erase NVS credentials and trigger reboot.

#### [MODIFY] [dogbot_v1/main/main.c](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/dogbot_v1/main/main.c)
- Add boot button (`BTN_BOOT_GPIO`) monitoring task supporting 7-second hold WiFi reset countdown & confirmation.

#### [NEW] [dogbot_v1/main/resettingwifi.md](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/dogbot_v1/main/resettingwifi.md)
#### [NEW] [speakerbot/main/resettingwifi.md](file:///c:/Users/dontm/Documents/mojCodexstuff/ActiveGithub/robot-fleet/firmware/platforms/speakerbot/main/resettingwifi.md)
- Add user documentation explaining how to hold the boot button for 7 seconds to reset WiFi credentials across the fleet.

---

## Verification Plan

### Automated / Code Inspection
- Verify all `board_config.h` files contain `#define FW_VERSION "v1.9"`.
- Verify every platform `main/` folder contains a separate `index.html` file.
- Verify every `CMakeLists.txt` has `EMBED_TXTFILES "index.html"`.
- Verify `wifi_forget_all()` is declared and implemented across all `wifi_mgr` modules.
- Check syntax and consistency across all modified C and HTML files.

### Manual Verification D
- DO NOT BUILD OR FLASH I WILL DO THAT MYSELF

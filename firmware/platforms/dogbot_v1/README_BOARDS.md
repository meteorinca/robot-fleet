# README_BOARDS.md — Multi-Board Build System

This project supports multiple ESP32 boards from a **single codebase**. 
No branches, no duplicated files — just one command to switch.

---

## How to build for each board



### ESP32-C3 Dog Robot (4 leg servos)

```powershell
idf.py -DBOARD=esp32c3_dog set-target esp32c3 fullclean build flash monitor
```

> **`fullclean` is required when switching boards or chips.**  
> It deletes the cached `sdkconfig` so the new chip's defaults apply cleanly.

---

## How it works

```
project/
├── CMakeLists.txt          ← Reads -DBOARD=, sets BOARD_CONFIG_HEADER
├── sdkconfig.defaults      ← Common SDK settings (all boards)
├── sdkconfig.defaults.esp32s3  ← S3-specific SDK settings (auto-loaded by IDF)
├── sdkconfig.defaults.esp32c3  ← C3-specific SDK settings (auto-loaded by IDF)
│
├── boards/
│   └── esp32c3_dog/
│       └── board_config.h  ← ALL pin defs, feature flags for this board
│
└── main/
    ├── config.h            ← Thin router: #include BOARD_CONFIG_HEADER
    ├── servo.c             ← Uses SERVO_COUNT — works for 2 or 4 servos
    ├── rf.c                ← Only compiled when board has RF_RX_GPIO defined
    └── touch_input.c       ← Entire body guarded by SOC_TOUCH_SENSOR_SUPPORTED
```

### Feature gates used

| Mechanism | What it controls |
|-----------|-----------------|
| `SERVO_COUNT` (board_config.h) | How many servos init'd and exposed via HTTP |
| `#ifdef RF_RX_GPIO` | RF module code and `/send` HTTP endpoint |
| `SOC_TOUCH_SENSOR_SUPPORTED` | Touch pad task (auto-false on C3) |
| `CMakeLists.txt IN_LIST HAS_RF_BOARDS` | Whether `rf.c` is even compiled |
| `sdkconfig.defaults.<chip>` | Chip-specific SDK config (USB CDC, UART, etc.) |

---

## Adding a new board

1. Create `boards/<your_board_name>/board_config.h`  
   Copy the nearest existing one and edit pins/features.
2. If your board has RF: add `"your_board_name"` to `HAS_RF_BOARDS` in `main/CMakeLists.txt`.
3. Build: `idf.py -DBOARD=<your_board_name> set-target <chip> fullclean build`

That's it. No branching, no duplicated source files.

---

## Why not Git branches?

Branches are for **features and experiments**, not for board variants that 
share the same logic. Putting board variants in branches means:

- Bug fixes must be cherry-picked to every branch
- You can't easily `diff` two boards
- CI must build N branches separately

The **board config header pattern** (used by ESP-IDF's own `esp-bsp` and `esp-box` 
projects) keeps everything in one branch, one history, one PR.

Use branches for: new features, experimental refactors, release freeze.  
Use `boards/` for: hardware variants.

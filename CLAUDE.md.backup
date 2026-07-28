# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP-IDF / LVGL firmware for the 学而思 (Xueersi) **小喵掌机** (Xiaomiao handheld): an ESP32-WROVER-B main MCU plus a GD32F350G8 co-processor. The ESP32 firmware is a hardware-status dashboard (LVGL UI) that drives the screen, reads sensors, and talks to the GD32 and MPU6050 over a shared I2C bus. Hardware reference notes, pin tables, and the GD32 I2C protocol live in `README.md`.

Key hardware facts (see README for full pin tables):
- ESP32 is main controller (UI, sensors, SD, buttons); GD32 is USB-CDC serial bridge, ESP32 auto-reset/download controller, and I2C slave `0x40` driving LEDs + motors.
- Display is a 160×128 ST7735 on SPI2 (40 MHz); **the screen TE pin is not connected** so vsync/anti-tear is impossible, and **backlight is tied to the GD32** so brightness is not adjustable from ESP32.
- TFT and MicroSD share SPI2 with separate CS pins (TFT CS=GPIO5, SD CS=GPIO22).
- GPIO15/GPIO21 are shared between I2C (SCL/SDA) and UART1 (SugarASR) — they are mutually exclusive.

## Build / Flash / Monitor

This is an ESP-IDF v5.5.4 project (target `esp32`). The devcontainer in `.devcontainer/` is based on `espressif/idf` and sources `/opt/esp/idf/export.sh`. In a local shell you must have ESP-IDF activated first (`. $IDF_PATH/export.sh`).

```bash
idf.py set-target esp32          # first time only
idf.py build                     # builds; managed_components/lvgl pulled from dependencies.lock
idf.py -p /dev/ttyACM0 -b 460800 flash monitor   # flash + serial monitor
idf.py -p /dev/ttyACM0 monitor                    # monitor only
```

- `sdkconfig.defaults` pins the tuning: CPU 240 MHz, QIO flash @ 80 MHz, 4 MB flash, PSRAM quad @ 80 MHz, FreeRTOS 1000 Hz, LVGL 9.5 with RGB565 swapped, 16 ms refresh period. A committed `sdkconfig` is gitignored — the defaults are the source of truth; run `idf.py build` to materialize it.
- LVGL is pulled via the component manager (`main/idf_component.yml` → `lvgl/lvgl: "9.5.0"`, locked in `dependencies.lock`). Never add LVGL under `components/`; update the version in the yml + lock, not by vendoring.
- To reflash just-built binaries without rebuilding: `esptool.py --chip esp32 -b 460800 write_flash 0x0 build/xiaomiao-merged.bin` (or use `idf.py merge-bin` to produce the merged image).

There are **no unit tests** in this repo. Verification is by flashing to hardware and reading the serial monitor / observing the UI.

## VS Code / Toolchain paths

`.vscode/settings.json` is hard-coded to the original author's machine (`/home/zyoung/...` paths for `idf.currentSetup`, `idf.port=/dev/ttyACM0`, OpenOCD `esp32-wrover-kit-3.3v.cfg`, and clangd). `c_cpp_properties.json` relies on `${config:idf.buildPath}/compile_commands.json`. If working on a different machine, update these paths; `.clangd` strips `-f*`/`-m*` flags so clangd works without a full IDF compile-commands DB.

## ESP32 Firmware Architecture (`main/main.c`)

The entire ESP32 app is a single ~2200-line file, `main/main.c`, registered as the sole component source in `main/CMakeLists.txt` (links `m` for math). There are no other translation units on the ESP32 side. Understand the structure by section:

- **Pins & tuning** (top of file, `PIN_NUM_*`, `LCD_*`, `GD32_*_REG`, `MPU6050_*`): all hardware pin assignments and I2C register constants. The README pin tables and these `#define`s are the two sources of truth — keep them in sync.
- **`board_state_t`** (`s_board`): single global struct holding all runtime hardware state (presence flags, raw sensor values, motor/LED/ext-IO state, last `esp_err_t` per peripheral, on-screen `action` string). Hardware code writes here; UI code reads here.
- **`ui_state_t`** (`s_ui`): all LVGL widget handles for the current page.
- **`ui_page_t`** enum + `s_page_names[]`: the dashboard pages navigated with D-pad (LIGHT, THERM, MOTION, LED1/2, BUZZER, MOTOR1/2, SD, GPIO25/26, GPIO32/33, SYSTEM, ABOUT).
- **Hardware layer**: `hardware_init()` / `i2c_init()` / `adc_init()` / `ext_io_init()` / `buzzer_init()` / `buttons_init()` / `lcd_init()` bring up peripherals; `hardware_update()` polls sensors + I2C devices and fills `s_board`; `i2c_probe_devices()` / `gd32_probe()` / `mpu_probe_and_init()` re-probe absent devices on a timer (`GD32_REPROBE_PERIOD_MS`, `MPU_REPROBE_PERIOD_MS`) so hot-plugged MPU6050s / a GD32 that comes online are picked up.
- **GD32 protocol** (`gd32_*`): LEDs via memory-write to regs `0xA0`/`0xA1`; motors via 8-byte PWM register block starting at `0x06` (Motor 2) / `0x0E` (Motor 1), with speed `0–255` scaled to 12-bit as `speed << 4`. `README.md` §5 documents the wire format in detail.
- **ST7735 driver** (`st7735_*`, `lcd_display_on`): manual init sequence + black-tab rotation handling; display is left **off** until the first LVGL flush completes (`s_lcd_first_flush_done`) to avoid garbage-frame flicker — see `lvgl_task`.
- **LVGL layer**: `lvgl_display_init` wires triple-buffered draw buffers (`LCD_DRAW_BUF_COUNT = 3`, DMA-allocated via `spi_bus_dma_memory_alloc`), `lvgl_flush_cb` pushes pixels, `lcd_flush_ready_cb` signals LVGL on SPI trans-complete (ISR-driven, hence `SPI_MASTER_IN_IRAM` in sdkconfig). `keypad_read_cb` maps the 6 GPIO buttons (`s_buttons[]`) to `LV_KEY_*` via an `lv_group_t`. `lvgl_tick_cb` is a 1 ms periodic `esp_timer`.
- **UI layer**: `ui_create` builds all pages; `ui_show_page(page, dir)` animates between pages; `ui_refresh()` republishes `s_board` → visible widgets every `UI_REFRESH_PERIOD_MS` (16 ms → ~60 fps). `ui_action()`/`ui_cancel()`/`ui_adjust()` handle A/B-button actions per page (toggle LED, run/stop motor, beep, toggle ext-IO). LIGHT and THERM pages keep rolling history arrays (`s_light_history`/`s_therm_history`, `UI_HISTORY_POINTS`) shown as LVGL line charts.

Runtime model: `app_main` does all init, starts the LVGL tick timer, and spawns a single `lvgl_task` (10 KB stack, priority 5). That task is the only worker — it loops `hardware_process_timers()` → (every 16 ms) `hardware_update()` + `ui_refresh()` → `lv_timer_handler()`. There is no separate sensor task; do not introduce concurrency here without a reason, since LVGL access must stay single-threaded.

## GD32 Firmware (`GD32_firmware/`)

Separate MCU, built with **Keil MDK-ARM** (`Project/MDK-ARM/cdc_acm.uvprojx`) or GD32EBuilder (`Project/GD32EBuilder_project/`). Source files are GB2312-encoded (see `GD32_firmware/.vscode/settings.json`: `"files.encoding": "gb2312"`) — open/edit with that encoding or comments become mojibake. `app.c` is a USB CDC loopback/bridge: dual ring buffers (`pc_to_esp_buf` / `esp_to_pc_buf`, `BUF_SIZE=2048`) shuttle bytes between USB and USART1, and DTR/RTS lines control ESP32 IO0/EN for auto-download (see `esp32_hardware_init`, `ESP32_IO0_PIN`/`ESP32_EN_PIN`). The GD32 firmware is WIP — README notes the LED/motor/ESP32 control protocol is still being finalized.

## `go.py`

A MicroPython probe script (not part of the build) that runs **on the device** to introspect the original 小喵掌机 MicroPython runtime (`meowbit`, `motor`, `sensor` objects) and reverse-engineer the I2C motor protocol via a `FakeI2C` shim. It documents how the wire-format constants in `main.c` were derived; reference only, do not run it from this host.

## Conventions

- ESP32 C uses ESP-IDF v5.x new-style APIs (`i2c_master_*`, `esp_adc/adc_oneshot`, `esp_lcd_panel_io`, `ledc`). Match these — do not regress to legacy `driver/i2c.h` or `adc_continuous` unless needed.
- All hardware tuning lives in `sdkconfig.defaults`; LVGL config is also driven from there (`CONFIG_LV_*`), not from an `lv_conf.h` (note `CONFIG_LV_CONF_SKIP=y`).
- The README is bilingual Chinese/English and doubles as the hardware spec. Pin or protocol changes must be reflected in both `README.md` and the matching `#define`s in `main.c`.
- Hardware-modification support is explicitly out of scope: this project targets stock factory hardware only. Hardware-mod work belongs in a separate branch/fork (see README "参与项目").

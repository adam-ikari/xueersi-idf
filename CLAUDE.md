# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP-IDF / SDL3 firmware for the 学而思 (Xueersi) **小喵掌机** (Xiaomiao handheld): an ESP32-WROVER-B main MCU plus a GD32F350G8 co-processor. The ESP32 firmware is an SDL3 demo that drives the screen, reads sensors, and talks to the GD32 and MPU6050 over a shared I2C bus. Hardware reference notes, pin tables, and the GD32 I2C protocol live in `README.md`.

Key hardware facts (see README for full pin tables):
- ESP32 is main controller (UI, sensors, SD, buttons); GD32 is USB-CDC serial bridge, ESP32 auto-reset/download controller, and I2C slave `0x40` driving LEDs + motors.
- Display is a 160×128 ST7735 on SPI2 (40 MHz); **the screen TE pin is not connected** so vsync/anti-tear is impossible, and **backlight is tied to the GD32** so brightness is not adjustable from ESP32.
- TFT and MicroSD share SPI2 with separate CS pins (TFT CS=GPIO5, SD CS=GPIO22).
- GPIO15/GPIO21 are shared between I2C (SCL/SDA) and UART1 (SugarASR) — they are mutually exclusive.

## Build / Flash / Monitor

This is an ESP-IDF v5.5.4 project (target `esp32`). The devcontainer in `.devcontainer/` is based on `espressif/idf` and sources `/opt/esp/idf/export.sh`. In a local shell you must have ESP-IDF activated first (`. $IDF_PATH/export.sh`).

```bash
idf.py set-target esp32          # first time only
idf.py build                     # builds; managed_components/georgik__sdl pulled from dependencies.lock
idf.py -p /dev/ttyACM0 -b 460800 flash monitor   # flash + serial monitor
idf.py -p /dev/ttyACM0 monitor                    # monitor only
```

- `sdkconfig.defaults` pins the tuning: CPU 240 MHz, QIO flash @ 80 MHz, 4 MB flash, PSRAM quad @ 80 MHz, FreeRTOS 1000 Hz, SDL3 with RGB565 swapped, 16 ms refresh period. A committed `sdkconfig` is gitignored — the defaults are the source of truth; run `idf.py build` to materialize it.
- SDL3 is pulled via the component manager (`main/idf_component.yml` → `georgik/sdl: "3.2.10"`, locked in `dependencies.lock`). Never add SDL under `components/`; update the version in the yml + lock, not by vendoring.
- To reflash just-built binaries without rebuilding: `esptool.py --chip esp32 -b 460800 write_flash 0x0 build/xiaomiao-merged.bin` (or use `idf.py merge-bin` to produce the merged image).

There are **no unit tests** in this repo. Verification is by flashing to hardware and reading the serial monitor / observing the UI.

## VS Code / Toolchain paths

`.vscode/settings.json` is hard-coded to the original author's machine (`/home/zyoung/...` paths for `idf.currentSetup`, `idf.port=/dev/ttyACM0`, OpenOCD `esp32-wrover-kit-3.3v.cfg`, and clangd). `c_cpp_properties.json` relies on `${config:idf.buildPath}/compile_commands.json`. If working on a different machine, update these paths; `.clangd` strips `-f*`/`-m*` flags so clangd works without a full IDF compile-commands DB.

## ESP32 Firmware Architecture

### `main/main.c`

The entry point is a slim ~50-line file, `main/main.c`, registered as the sole component source in `main/CMakeLists.txt` (links `m` for math). It calls `hw_board_init()` from the hardware abstraction layer, then creates and runs the SDL3 demo task.

### `main/sdl_demo.c`

`sdl_demo.c` is the 15-page SDL3 demo. It sets up an SDL window/renderer, handles input via the hardware abstraction layer, and renders a multi-page demo UI.

### `components/hardware/`

The hardware abstraction layer (HAL) for the Xiaomiao board. Key files:
- `hw_board.c` / `hw_board.h`: board initialization, pin assignments, and `hw_board_init()`.
- `hw_display.c` / `hw_display.h`: ST7735 display driver (SPI2, 40 MHz, 160×128, RGB565 swapped).
- `hw_input.c` / `hw_input.h`: GPIO button reading (6 buttons mapped to SDL scancodes).
- `hw_i2c.c` / `hw_i2c.h`: I2C master for GD32 and MPU6050.
- `hw_gd32.c` / `hw_gd32.h`: GD32 I2C slave protocol (LEDs, motors, ESP32 control).
- `hw_mpu6050.c` / `hw_mpu6050.h`: MPU6050 accelerometer/gyroscope driver.
- `hw_sd.c` / `hw_sd.h`: MicroSD card over shared SPI2.
- `hw_buzzer.c` / `hw_buzzer.h`: passive buzzer via PWM.
- `hw_adc.c` / `hw_adc.h`: light sensor and thermistor ADC readings.

### `components/georgik__sdl_bsp/`

The board support package (BSP) adapter that bridges the hardware layer to SDL3. It provides the SDL video driver for the ST7735 and the input driver for GPIO buttons.

### Key architectural notes

- **Pins & tuning**: all hardware pin assignments and I2C register constants live in `components/hardware/hw_board.h`. The README pin tables and these `#define`s are the two sources of truth — keep them in sync.
- **GD32 protocol** (`hw_gd32.c`): LEDs via memory-write to regs `0xA0`/`0xA1`; motors via 8-byte PWM register block starting at `0x06` (Motor 2) / `0x0E` (Motor 1), with speed `0–255` scaled to 12-bit as `speed << 4`. `README.md` §5 documents the wire format in detail.
- **ST7735 driver** (`hw_display.c`): manual init sequence + black-tab rotation handling; display is left **off** until the first SDL flush completes to avoid garbage-frame flicker.
- **SDL3 layer**: SDL3 handles its own render loop and event polling. The hardware layer feeds button events into SDL's event queue via the BSP.

Runtime model: `app_main` does all init and spawns the SDL demo task. There is no separate sensor task; hardware polling is done from the demo task loop.

**Note**: `managed_components/georgik__sdl/CMakeLists.txt` and `SDL_udev.h` stub are patched for ESP32 — re-apply patches if reconfiguring.

## GD32 Firmware (`GD32_firmware/`)

Separate MCU, built with **Keil MDK-ARM** (`Project/MDK-ARM/cdc_acm.uvprojx`) or GD32EBuilder (`Project/GD32EBuilder_project/`). Source files are GB2312-encoded (see `GD32_firmware/.vscode/settings.json`: `"files.encoding": "gb2312"`) — open/edit with that encoding or comments become mojibake. `app.c` is a USB CDC loopback/bridge: dual ring buffers (`pc_to_esp_buf` / `esp_to_pc_buf`, `BUF_SIZE=2048`) shuttle bytes between USB and USART1, and DTR/RTS lines control ESP32 IO0/EN for auto-download (see `esp32_hardware_init`, `ESP32_IO0_PIN`/`ESP32_EN_PIN`). The GD32 firmware is WIP — README notes the LED/motor/ESP32 control protocol is still being finalized.

## `go.py`

A MicroPython probe script (not part of the build) that runs **on the device** to introspect the original 小喵掌机 MicroPython runtime (`meowbit`, `motor`, `sensor` objects) and reverse-engineer the I2C motor protocol via a `FakeI2C` shim. It documents how the wire-format constants in `main.c` were derived; reference only, do not run it from this host.

## Conventions

- ESP32 C uses ESP-IDF v5.x new-style APIs (`i2c_master_*`, `esp_adc/adc_oneshot`, `esp_lcd_panel_io`, `ledc`). Match these — do not regress to legacy `driver/i2c.h` or `adc_continuous` unless needed.
- All hardware tuning lives in `sdkconfig.defaults`; SDL3 config is also driven from there (`CONFIG_SDL_*`), not from an `sdl_config.h`.
- The README is bilingual Chinese/English and doubles as the hardware spec. Pin or protocol changes must be reflected in both `README.md` and the matching `#define`s in `hw_board.h`.
- Hardware-modification support is explicitly out of scope: this project targets stock factory hardware only. Hardware-mod work belongs in a separate branch/fork (see README "参与项目").

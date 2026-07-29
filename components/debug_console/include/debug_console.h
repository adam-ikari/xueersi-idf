#pragma once

/**
 * adb-like serial debug console for the Xiaomiao ESP32.
 *
 * Spawns an ESP-IDF REPL on UART0 (GPIO1/3, the console UART — separate from
 * the I2C-shared UART1). Provides commands to probe the live TinyGL state
 * (framebuffer pixels, texture pixmap, GL config), drive the display with
 * test patterns, and read FPS — for debugging the 3D pipeline without a
 * debugger attached.
 *
 * Call debug_console_start() once from app_main after the render task is up.
 */
void debug_console_start(void);

/**
 * @file sdl_demo.c
 * @brief SDL3 minimal demo for Xiaomiao handheld
 *
 * Proof-of-concept: renders "SDL3 OK" + live ADC light value.
 * Proves SDL3 video + input backends work via the Xiaomiao BSP adapter.
 */

#include "sdl_demo.h"

#include "hw_board.h"
#include "hw_adc.h"
#include "hw_display.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdl_demo";

// SDL3 globals
static SDL_Window *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;

// Colors
static const SDL_Color COLOR_BG      = {0x1B, 0x17, 0x13, 0xFF};  // UI_BLACK
static const SDL_Color COLOR_TEXT    = {0xF6, 0xD3, 0x4A, 0xFF};  // UI_YELLOW
static const SDL_Color COLOR_ACCENT  = {0xE6, 0x4B, 0x3C, 0xFF};  // UI_RED

void sdl_demo_create(void)
{
    ESP_LOGI(TAG, "SDL3 demo: creating");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        ESP_LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return;
    }

    s_window = SDL_CreateWindow("Xiaomiao", 160, 128, 0);
    if (!s_window) {
        ESP_LOGE(TAG, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return;
    }

    s_renderer = SDL_CreateRenderer(s_window, NULL);
    if (!s_renderer) {
        ESP_LOGE(TAG, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(s_window);
        SDL_Quit();
        return;
    }

    ESP_LOGI(TAG, "SDL3 demo: window + renderer created (160x128)");
}

void sdl_demo_task(void *arg)
{
    (void)arg;
    uint32_t last_update_ms = 0;

    // Wait for first display flush to complete, then turn on display
    // (Same anti-flicker logic as original LVGL path)
    for (int i = 0; i < 100 && !hw_display_first_flush_done(); ++i) {
        SDL_Delay(1);
    }
    hw_display_on();

    ESP_LOGI(TAG, "SDL3 demo: task starting main loop");

    while (true) {
        uint32_t now = SDL_GetTicks();
        uint32_t elapsed = now - last_update_ms;

        // 16ms refresh cycle (~60fps)
        if (elapsed >= 16) {
            last_update_ms = now;

            // Update hardware state
            hw_board_process_timers();
            hw_board_update();

            // Poll SDL events
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    ESP_LOGI(TAG, "SDL_QUIT received");
                    goto done;
                case SDL_EVENT_KEY_DOWN:
                    ESP_LOGI(TAG, "Key down: scancode=%d key=%d",
                             event.key.scancode, event.key.key);
                    break;
                case SDL_EVENT_KEY_UP:
                    break;
                default:
                    break;
                }
            }

            // --- Rendering ---

            // Clear to black
            SDL_SetRenderDrawColor(s_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
            SDL_RenderClear(s_renderer);

            // Title text
            SDL_SetRenderDrawColor(s_renderer, COLOR_TEXT.r, COLOR_TEXT.g, COLOR_TEXT.b, COLOR_TEXT.a);
            SDL_RenderDebugText(s_renderer, 4.0f, 4.0f, "Xiaomiao SDL3 Demo");

            // Divider
            SDL_SetRenderDrawColor(s_renderer, COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, COLOR_ACCENT.a);
            SDL_RenderLine(s_renderer, 0.0f, 18.0f, 160.0f, 18.0f);

            // Light value
            const hw_board_state_t *board = hw_board_state();
            char buf[64];
            int len = snprintf(buf, sizeof(buf), "Light: %4d (%3d%%)",
                               board->light_raw,
                               board->light_raw * 100 / 4095);
            SDL_SetRenderDrawColor(s_renderer, COLOR_TEXT.r, COLOR_TEXT.g, COLOR_TEXT.b, COLOR_TEXT.a);
            SDL_RenderDebugText(s_renderer, 4.0f, 24.0f, buf);

            // Temperature
            len = snprintf(buf, sizeof(buf), "Temp:  %4d (%3d%%)",
                           board->temp_raw,
                           board->temp_raw * 100 / 4095);

            SDL_RenderDebugText(s_renderer, 4.0f, 36.0f, buf);

            // GD32 status
            len = snprintf(buf, sizeof(buf), "GD32:  %s",
                           board->gd32_present ? "ONLINE" : "ABSENT");
            SDL_RenderDebugText(s_renderer, 4.0f, 52.0f, buf);

            // MPU status
            len = snprintf(buf, sizeof(buf), "MPU:   %s",
                           board->mpu_present ? "ONLINE" : "ABSENT");
            SDL_RenderDebugText(s_renderer, 4.0f, 64.0f, buf);

            // SD status
            len = snprintf(buf, sizeof(buf), "SD:    %s",
                           board->sd_mounted ? board->sd_name : "NO CARD");
            SDL_RenderDebugText(s_renderer, 4.0f, 76.0f, buf);

            // Ticks counter
            len = snprintf(buf, sizeof(buf), "Frame: %lu", (unsigned long)board->samples);
            SDL_RenderDebugText(s_renderer, 4.0f, 92.0f, buf);

            // Action hint
            if (board->action[0]) {
                SDL_SetRenderDrawColor(s_renderer, COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, COLOR_ACCENT.a);
                SDL_RenderDebugText(s_renderer, 4.0f, 110.0f, board->action);
            }

            // Present
            SDL_RenderPresent(s_renderer);
        }

        uint32_t delay = 16 - (SDL_GetTicks() - last_update_ms);
        if (delay > 16) delay = 16;
        if (delay > 0) SDL_Delay(delay);
    }

done:
    ESP_LOGI(TAG, "SDL3 demo: exiting");
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
    vTaskDelete(NULL);
}
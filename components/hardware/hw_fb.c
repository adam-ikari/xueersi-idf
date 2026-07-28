#include "hw_fb.h"
#include "hw_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "hw_fb";
static uint16_t *s_fb = NULL;

#define FB_WIDTH  160
#define FB_HEIGHT 128
#define FB_SIZE   (FB_WIDTH * FB_HEIGHT * sizeof(uint16_t))

void hw_fb_init(void)
{
    if (s_fb != NULL) {
        return;
    }

    s_fb = (uint16_t *)heap_caps_malloc(FB_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer in PSRAM");
        return;
    }

    memset(s_fb, 0, FB_SIZE);
    ESP_LOGI(TAG, "Framebuffer allocated: %dx%d RGB565 in PSRAM", FB_WIDTH, FB_HEIGHT);
}

void hw_fb_clear(uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }

    for (int i = 0; i < FB_WIDTH * FB_HEIGHT; ++i) {
        s_fb[i] = color;
    }
}

void hw_fb_set_pixel(int x, int y, uint16_t color)
{
    if (s_fb == NULL) {
        return;
    }
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) {
        return;
    }

    s_fb[y * FB_WIDTH + x] = color;
}

uint16_t hw_fb_get_pixel(int x, int y)
{
    if (s_fb == NULL) {
        return 0;
    }
    if (x < 0 || x >= FB_WIDTH || y < 0 || y >= FB_HEIGHT) {
        return 0;
    }

    return s_fb[y * FB_WIDTH + x];
}

void hw_fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (s_fb == NULL || w <= 0 || h <= 0) {
        return;
    }

    int x1 = x;
    int y1 = y;
    int x2 = x + w - 1;
    int y2 = y + h - 1;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= FB_WIDTH) x2 = FB_WIDTH - 1;
    if (y2 >= FB_HEIGHT) y2 = FB_HEIGHT - 1;

    if (x1 > x2 || y1 > y2) {
        return;
    }

    for (int row = y1; row <= y2; ++row) {
        for (int col = x1; col <= x2; ++col) {
            s_fb[row * FB_WIDTH + col] = color;
        }
    }
}

void hw_fb_blit(int x, int y, int w, int h, const uint16_t *data)
{
    if (s_fb == NULL || data == NULL || w <= 0 || h <= 0) {
        return;
    }

    for (int row = 0; row < h; ++row) {
        int dst_y = y + row;
        if (dst_y < 0 || dst_y >= FB_HEIGHT) {
            continue;
        }
        for (int col = 0; col < w; ++col) {
            int dst_x = x + col;
            if (dst_x < 0 || dst_x >= FB_WIDTH) {
                continue;
            }
            s_fb[dst_y * FB_WIDTH + dst_x] = data[row * w + col];
        }
    }
}

void hw_fb_flush(void)
{
    if (s_fb == NULL) {
        return;
    }

    hw_display_flush(0, 0, FB_WIDTH - 1, FB_HEIGHT - 1, (uint8_t *)s_fb);
}

uint16_t *hw_fb_buffer(void)
{
    return s_fb;
}

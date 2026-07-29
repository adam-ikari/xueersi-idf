#include "emu_bmp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma pack(push, 1)

typedef struct {
    uint16_t type;      /* 'BM' */
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;    /* 54 for 24-bit */
} bmp_file_header_t;

typedef struct {
    uint32_t size;      /* 40 */
    int32_t  width;
    int32_t  height;
    uint16_t planes;    /* 1 */
    uint16_t bpp;       /* 24 */
    uint32_t compression; /* 0 = BI_RGB */
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;

#pragma pack(pop)

/* Convert RGB565 big-endian (ESP32 format) to R8G8B8 */
static inline void rgb565_to_rgb888(uint16_t pix, uint8_t* r, uint8_t* g, uint8_t* b) {
    /* pix is big-endian RGB565: high byte = RRRRRGGG, low byte = GGGBBBBB */
    uint16_t p = ((pix & 0xFF) << 8) | ((pix >> 8) & 0xFF);  /* swap to little-endian */
    *r = (uint8_t)((p >> 11) & 0x1F) << 3;
    *g = (uint8_t)((p >> 5) & 0x3F) << 2;
    *b = (uint8_t)(p & 0x1F) << 3;
}

int bmp_save_rgb565(const uint16_t* fb, int w, int h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    int row_size = ((w * 3 + 3) / 4) * 4;  /* padded to 4-byte boundary */
    int image_size = row_size * h;

    bmp_file_header_t fh = {
        .type = 0x4D42,  /* 'BM' */
        .size = 54 + image_size,
        .reserved1 = 0,
        .reserved2 = 0,
        .offset = 54,
    };

    bmp_info_header_t ih = {
        .size = 40,
        .width = w,
        .height = h,
        .planes = 1,
        .bpp = 24,
        .compression = 0,
        .image_size = image_size,
        .x_ppm = 2835,  /* 72 DPI */
        .y_ppm = 2835,
        .colors_used = 0,
        .colors_important = 0,
    };

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    uint8_t* row = (uint8_t*)malloc(row_size);
    if (!row) { fclose(f); return -1; }
    memset(row, 0, row_size);

    /* BMP rows are bottom-up */
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t pix = fb[y * w + x];
            uint8_t r, g, b;
            rgb565_to_rgb888(pix, &r, &g, &b);
            /* BMP uses BGR order */
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, row_size, 1, f);
    }

    free(row);
    fclose(f);
    return 0;
}

/* 16-bit RGB565 raw BMP (BI_BITFIELDS) */
int bmp_save_rgb565_raw(const uint16_t* fb, int w, int h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    int row_size = ((w * 2 + 3) / 4) * 4;
    int image_size = row_size * h;

    bmp_file_header_t fh = {
        .type = 0x4D42,
        .size = 54 + 12 + image_size,  /* +12 for bitmasks */
        .reserved1 = 0,
        .reserved2 = 0,
        .offset = 54 + 12,
    };

    bmp_info_header_t ih = {
        .size = 40,
        .width = w,
        .height = h,
        .planes = 1,
        .bpp = 16,
        .compression = 3,  /* BI_BITFIELDS */
        .image_size = image_size,
        .x_ppm = 2835,
        .y_ppm = 2835,
        .colors_used = 0,
        .colors_important = 0,
    };

    uint32_t masks[3] = {0xF800, 0x07E0, 0x001F};  /* R, G, B masks */

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(masks, sizeof(masks), 1, f);

    uint8_t* row = (uint8_t*)malloc(row_size);
    if (!row) { fclose(f); return -1; }
    memset(row, 0, row_size);

    for (int y = h - 1; y >= 0; y--) {
        memcpy(row, &fb[y * w], w * 2);
        /* Swap bytes per pixel to little-endian for BMP */
        for (int x = 0; x < w; x++) {
            uint8_t tmp = row[x * 2];
            row[x * 2] = row[x * 2 + 1];
            row[x * 2 + 1] = tmp;
        }
        fwrite(row, row_size, 1, f);
    }

    free(row);
    fclose(f);
    return 0;
}

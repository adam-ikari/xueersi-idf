#include "test_rasterizer.h"
#include "zbuffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_W 160
#define TEST_H 128

/* Color values in the format TinyGL's rasterizer expects (high-byte format).
 * glColor4f(1.0,0.0,0.0) computes: ((1.0 * 0xFE0000) + 0x00FFFF) & 0xFFFFFF = 0xFEFFFF
 * glColor4f(0.0,0.0,0.0) computes: ((0.0 * 0xFE0000) + 0x00FFFF) & 0xFFFFFF = 0x00FFFF
 */
#define GLCOLOR(v) ((((unsigned int)((v) * 0xFE0000)) + 0x00FFFF) & 0xFFFFFF)
#define COLOR_FULL GLCOLOR(1.0f)
#define COLOR_ZERO GLCOLOR(0.0f)

static ZBuffer* create_test_zb(void) {
    void* fb = malloc(TEST_W * TEST_H * 2);
    if (!fb) return NULL;
    return ZB_open(TEST_W, TEST_H, ZB_MODE_5R6G5B, fb);
}

static void destroy_test_zb(ZBuffer* zb) {
    if (zb) {
        free(zb->pbuf);
        ZB_close(zb);
    }
}

/*
 * Point-in-triangle test using barycentric cross products.
 * Returns 1 if (x,y) is inside or on the edge of the triangle.
 */
static int point_in_triangle(int x, int y,
                             int x0, int y0,
                             int x1, int y1,
                             int x2, int y2) {
    long long ax = x0 - x, ay = y0 - y;
    long long bx = x1 - x, by = y1 - y;
    long long cx = x2 - x, cy = y2 - y;

    long long cross1 = ax * by - ay * bx;
    long long cross2 = bx * cy - by * cx;
    long long cross3 = cx * ay - cy * ax;

    return (cross1 >= 0 && cross2 >= 0 && cross3 >= 0) ||
           (cross1 <= 0 && cross2 <= 0 && cross3 <= 0);
}

// 测试 1: Flat 三角形（纯色填充）
static test_result_t test_flat_triangle(void) {
    test_result_t r = {"flat_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_ZERO, (GLint)COLOR_ZERO, 0, 0};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_ZERO, (GLint)COLOR_ZERO, 0, 0};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_ZERO, (GLint)COLOR_ZERO, 0, 0};

    ZB_fillTriangleFlatNOBLEND(zb, &p0, &p1, &p2);

    uint16_t expected = RGB_TO_PIXEL(COLOR_FULL, COLOR_ZERO, COLOR_ZERO);
    int errors = 0;
    int inside_pixels = 0;
    int correct_inside = 0;

    for (int y = 10; y < 40; y++) {
        for (int x = 10; x < 50; x++) {
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (point_in_triangle(x, y, 10, 10, 50, 10, 30, 40)) {
                inside_pixels++;
                if (pix == expected) {
                    correct_inside++;
                } else {
                    errors++;
                }
            } else {
                /* Outside triangle: allow background */
                if (pix != 0 && pix != expected) {
                    errors++;
                }
            }
        }
    }

    /* Must have drawn at least some interior pixels correctly */
    int pass = (inside_pixels > 0) &&
               (correct_inside >= inside_pixels / 2) &&
               (errors < 5);

    r.diff_pixels = errors;
    r.passed = pass;
    if (!pass && !r.error_msg) {
        if (inside_pixels == 0) {
            r.error_msg = "no interior pixels found";
        } else if (correct_inside < inside_pixels / 2) {
            r.error_msg = "too few correct interior pixels";
        }
    }

    destroy_test_zb(zb);
    return r;
}

// 测试 2: Smooth 三角形（颜色插值）
static test_result_t test_smooth_triangle(void) {
    test_result_t r = {"smooth_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_ZERO, (GLint)COLOR_ZERO, 0, 0};   // 红
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, (GLint)COLOR_ZERO, (GLint)COLOR_FULL, (GLint)COLOR_ZERO, 0, 0};   // 绿
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, (GLint)COLOR_ZERO, (GLint)COLOR_ZERO, (GLint)COLOR_FULL, 0, 0}; // 蓝

    ZB_fillTriangleSmoothNOBLEND(zb, &p0, &p1, &p2);

    uint16_t red   = RGB_TO_PIXEL(COLOR_FULL, COLOR_ZERO, COLOR_ZERO);
    uint16_t green = RGB_TO_PIXEL(COLOR_ZERO, COLOR_FULL, COLOR_ZERO);
    uint16_t blue  = RGB_TO_PIXEL(COLOR_ZERO, COLOR_ZERO, COLOR_FULL);

    /* 验证：三个顶点颜色正确 */
    uint16_t c0 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 10];
    uint16_t c1 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 50];
    uint16_t c2 = ((uint16_t*)zb->pbuf)[40 * TEST_W + 30];

    int errors = 0;
    if (c0 != red) errors++;
    if (c1 != green) errors++;
    if (c2 != blue) errors++;

    /* 验证内部像素有插值颜色（不是纯顶点色） */
    uint16_t centroid = ((uint16_t*)zb->pbuf)[20 * TEST_W + 30];
    int centroid_ok = (centroid != red) && (centroid != green) && (centroid != blue) && (centroid != 0);
    if (!centroid_ok) errors++;

    r.diff_pixels = errors;
    r.passed = (errors == 0);

    destroy_test_zb(zb);
    return r;
}

// 测试 3: 纹理映射三角形
static test_result_t test_textured_triangle(void) {
    test_result_t r = {"textured_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    /* 创建 4x4 测试纹理（红色） */
    uint16_t tex[16];
    uint16_t red_texel = RGB_TO_PIXEL(COLOR_FULL, COLOR_ZERO, COLOR_ZERO);
    for (int i = 0; i < 16; i++) tex[i] = red_texel;
    ZB_setTexture(zb, tex);

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_FULL, (GLint)COLOR_FULL, 0, 0};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_FULL, (GLint)COLOR_FULL, 0, 0};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, (GLint)COLOR_FULL, (GLint)COLOR_FULL, (GLint)COLOR_FULL, 0, 0};
    /* 纹理坐标 */
    p0.s = 0; p0.t = 0;
    p1.s = 1 << ZB_POINT_S_FRAC_BITS; p1.t = 0;
    p2.s = 0; p2.t = 1 << ZB_POINT_T_FRAC_BITS;

    ZB_fillTriangleMappingPerspectiveNOBLEND(zb, &p0, &p1, &p2);

    uint16_t expected = red_texel;
    int errors = 0;
    int inside_pixels = 0;
    int textured_inside = 0;

    for (int y = 12; y < 38; y++) {
        for (int x = 15; x < 45; x++) {
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (point_in_triangle(x, y, 10, 10, 50, 10, 30, 40)) {
                inside_pixels++;
                if (pix == expected) {
                    textured_inside++;
                } else if (pix != 0) {
                    errors++;
                }
            } else {
                if (pix != 0 && pix != expected) {
                    errors++;
                }
            }
        }
    }

    /* Must have drawn at least some interior pixels with texture */
    int pass = (inside_pixels > 0) &&
               (textured_inside >= inside_pixels / 2) &&
               (errors < 10);

    r.diff_pixels = errors;
    r.passed = pass;
    if (!pass && !r.error_msg) {
        if (inside_pixels == 0) {
            r.error_msg = "no interior pixels found";
        } else if (textured_inside < inside_pixels / 2) {
            r.error_msg = "too few textured interior pixels";
        }
    }

    destroy_test_zb(zb);
    return r;
}

int test_rasterizer_run(test_result_t* results, int max_results) {
    int count = 0;
    int passed = 0;

    if (count < max_results) {
        results[count] = test_flat_triangle();
        if (results[count].passed) passed++;
        count++;
    }
    if (count < max_results) {
        results[count] = test_smooth_triangle();
        if (results[count].passed) passed++;
        count++;
    }
    if (count < max_results) {
        results[count] = test_textured_triangle();
        if (results[count].passed) passed++;
        count++;
    }

    return count;
}

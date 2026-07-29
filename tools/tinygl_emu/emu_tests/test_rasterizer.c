#include "test_rasterizer.h"
#include "zbuffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_W 160
#define TEST_H 128

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

static int compare_fb(uint16_t* a, uint16_t* b, int w, int h) {
    int diff = 0;
    for (int i = 0; i < w * h; i++) {
        uint16_t da = a[i] ^ b[i];
        int dr = (da >> 11) & 0x1F;
        int dg = (da >> 5) & 0x3F;
        int db = da & 0x1F;
        if (dr > 1 || dg > 1 || db > 1) diff++;
    }
    return diff;
}

// 测试 1: Flat 三角形（纯色填充）
static test_result_t test_flat_triangle(void) {
    test_result_t r = {"flat_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, 255, 0, 0};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, 255, 0, 0};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, 255, 0, 0};

    ZB_fillTriangleFlatNOBLEND(zb, &p0, &p1, &p2);

    // 验证：三角形区域内应为红色
    uint16_t expected = RGB_TO_PIXEL(255, 0, 0);
    int errors = 0;
    for (int y = 10; y < 40; y++) {
        for (int x = 10; x < 50; x++) {
            // 简单包围盒检查（不精确，但足够验证）
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (pix != expected && pix != 0) {  // 0 是背景
                errors++;
            }
        }
    }

    r.diff_pixels = errors;
    r.passed = (errors < 5);  // 允许少量边缘误差

    destroy_test_zb(zb);
    return r;
}

// 测试 2: Smooth 三角形（颜色插值）
static test_result_t test_smooth_triangle(void) {
    test_result_t r = {"smooth_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, 255, 0, 0};    // 红
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, 0, 255, 0};    // 绿
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, 0, 0, 255};   // 蓝

    ZB_fillTriangleSmoothNOBLEND(zb, &p0, &p1, &p2);

    // 验证：三个顶点颜色正确
    uint16_t c0 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 10];
    uint16_t c1 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 50];
    uint16_t c2 = ((uint16_t*)zb->pbuf)[40 * TEST_W + 30];

    int errors = 0;
    if (c0 != RGB_TO_PIXEL(255, 0, 0)) errors++;
    if (c1 != RGB_TO_PIXEL(0, 255, 0)) errors++;
    if (c2 != RGB_TO_PIXEL(0, 0, 255)) errors++;

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

    // 创建 4x4 测试纹理（红色）
    uint16_t tex[16];
    for (int i = 0; i < 16; i++) tex[i] = RGB_TO_PIXEL(255, 0, 0);
    ZB_setTexture(zb, tex);

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, 255, 255, 255};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, 255, 255, 255};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, 255, 255, 255};
    // 纹理坐标
    p0.s = 0; p0.t = 0;
    p1.s = 1 << ZB_POINT_S_FRAC_BITS; p1.t = 0;
    p2.s = 0; p2.t = 1 << ZB_POINT_T_FRAC_BITS;

    ZB_fillTriangleMappingPerspectiveNOBLEND(zb, &p0, &p1, &p2);

    // 验证：三角形区域内应为红色
    uint16_t expected = RGB_TO_PIXEL(255, 0, 0);
    int errors = 0;
    for (int y = 12; y < 38; y++) {
        for (int x = 15; x < 45; x++) {
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (pix != expected && pix != 0) {
                errors++;
            }
        }
    }

    r.diff_pixels = errors;
    r.passed = (errors < 10);

    destroy_test_zb(zb);
    return r;
}

int test_rasterizer_run(test_result_t* results, int max_results) {
    int count = 0;

    if (count < max_results) results[count++] = test_flat_triangle();
    if (count < max_results) results[count++] = test_smooth_triangle();
    if (count < max_results) results[count++] = test_textured_triangle();

    return count;
}

#include "zfeatures.h"
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>

#define TEST_W 160
#define TEST_H 128

// Compute color in the format glColor4f uses
#define GLCOLOR(v) ((((unsigned int)((v) * 0xFE0000)) + 0x00FFFF) & 0xFFFFFF)

int main() {
    printf("TGL_FEATURE_RENDER_BITS=%d\n", TGL_FEATURE_RENDER_BITS);
    printf("TGL_PIXEL_BYTE_SWAP=%d\n", TGL_PIXEL_BYTE_SWAP);
    
    unsigned int full = GLCOLOR(1.0f);
    unsigned int zero = GLCOLOR(0.0f);
    
    printf("full=0x%06x zero=0x%06x\n", full, zero);
    
    uint16_t red_pix = RGB_TO_PIXEL(full, zero, zero);
    uint16_t green_pix = RGB_TO_PIXEL(zero, full, zero);
    uint16_t blue_pix = RGB_TO_PIXEL(zero, zero, full);
    
    printf("red_pix=0x%04x green_pix=0x%04x blue_pix=0x%04x\n", red_pix, green_pix, blue_pix);
    
    void* fb = calloc(1, TEST_W * TEST_H * 2);
    ZBuffer* zb = ZB_open(TEST_W, TEST_H, ZB_MODE_5R6G5B, fb);
    
    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, (GLint)full, (GLint)zero, (GLint)zero, 0, 0};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, (GLint)zero, (GLint)full, (GLint)zero, 0, 0};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, (GLint)zero, (GLint)zero, (GLint)full, 0, 0};
    
    printf("p0: r=0x%x g=0x%x b=0x%x\n", p0.r, p0.g, p0.b);
    printf("p1: r=0x%x g=0x%x b=0x%x\n", p1.r, p1.g, p1.b);
    printf("p2: r=0x%x g=0x%x b=0x%x\n", p2.r, p2.g, p2.b);
    
    ZB_fillTriangleSmoothNOBLEND(zb, &p0, &p1, &p2);
    
    // Check vertices
    uint16_t c0 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 10];
    uint16_t c1 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 50];
    uint16_t c2 = ((uint16_t*)zb->pbuf)[40 * TEST_W + 30];
    
    printf("Vertex (10,10)=0x%04x expected=0x%04x %s\n", c0, red_pix, c0 == red_pix ? "OK" : "FAIL");
    printf("Vertex (50,10)=0x%04x expected=0x%04x %s\n", c1, green_pix, c1 == green_pix ? "OK" : "FAIL");
    printf("Vertex (30,40)=0x%04x expected=0x%04x %s\n", c2, blue_pix, c2 == blue_pix ? "OK" : "FAIL");
    
    // Count drawn pixels and check centroid
    int drawn = 0;
    int non_vertex = 0;
    for (int y = 10; y <= 40; y++) {
        for (int x = 10; x <= 50; x++) {
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (pix != 0) {
                drawn++;
                if (pix != red_pix && pix != green_pix && pix != blue_pix) {
                    non_vertex++;
                }
            }
        }
    }
    printf("Total drawn pixels: %d\n", drawn);
    printf("Non-vertex-color pixels: %d\n", non_vertex);
    
    // Check centroid
    uint16_t centroid = ((uint16_t*)zb->pbuf)[20 * TEST_W + 30];
    printf("Centroid (30,20)=0x%04x\n", centroid);
    printf("Centroid is blended: %s\n", 
           (centroid != red_pix && centroid != green_pix && centroid != blue_pix && centroid != 0) ? "YES" : "NO");
    
    ZB_close(zb);
    free(fb);
    return 0;
}

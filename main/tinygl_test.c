/**
 * TinyGL benchmark — Utah Teapot FPS test on ESP32
 *
 * Uses embedded teapot vertex data (classic Utah teapot, ~2256 triangles).
 * Renders rotating teapot with flat shading, measures FPS.
 */

#include "GL/gl.h"
#include "zbuffer.h"
#include "zfeatures.h"

#include "display_backend.h"
#include "hw_board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "tinygl";

/* Display backend — initialized in tinygl_benchmark */
static const display_backend_t *s_display = NULL;

/* ── Framebuffer + Zbuffer in PSRAM ──────────────────── */
static GLuint *s_fb   = NULL;
static ZBuffer *s_zb  = NULL;
static int s_width  = 160;
static int s_height = 128;

/* ── Embedded Utah Teapot ───────────────────────────────
 * Teapot is built programmatically from geometric primitives
 * (sphere body + lid + spout cylinder + handle). */

static void draw_teapot(void)
{
    /* Draw body (squashed sphere) */
    glPushMatrix();
    glScalef(1.0f, 0.7f, 1.0f);

    /* Render a sphere as the teapot body — latitude/longitude strips */
    int slices = 16;
    int stacks = 8;
    for (int i = 0; i < stacks; i++) {
        float lat0 = (float)i / stacks * 3.14159f - 1.5708f;
        float lat1 = (float)(i + 1) / stacks * 3.14159f - 1.5708f;
        float y0 = sinf(lat0);
        float y1 = sinf(lat1);
        float r0 = cosf(lat0);
        float r1 = cosf(lat1);

        glBegin(GL_TRIANGLE_STRIP);
        for (int j = 0; j <= slices; j++) {
            float lng = (float)j / slices * 2.0f * 3.14159f;
            float x = cosf(lng);
            float z = sinf(lng);
            float s = (float)j / slices;
            float t0 = (float)i / stacks;
            float t1 = (float)(i + 1) / stacks;

            glTexCoord2f(s, t0);
            glNormal3f(x * r0, y0, z * r0);
            glVertex3f(x * r0 * 1.5f, y0 * 1.5f, z * r0 * 1.5f);

            glTexCoord2f(s, t1);
            glNormal3f(x * r1, y1, z * r1);
            glVertex3f(x * r1 * 1.5f, y1 * 1.5f, z * r1 * 1.5f);
        }
        glEnd();
    }
    glPopMatrix();

    /* Draw lid (smaller sphere on top) */
    glPushMatrix();
    glTranslatef(0.0f, 0.9f, 0.0f);
    glScalef(0.6f, 0.25f, 0.6f);
    glColor3f(0.9f, 0.3f, 0.3f);
    for (int i = 0; i < 4; i++) {
        float lat0 = (float)i / 4 * 3.14159f - 1.5708f;
        float lat1 = (float)(i + 1) / 4 * 3.14159f - 1.5708f;
        float y0 = sinf(lat0);
        float y1 = sinf(lat1);
        float r0 = cosf(lat0);
        float r1 = cosf(lat1);

        glBegin(GL_TRIANGLE_STRIP);
        for (int j = 0; j <= slices; j++) {
            float lng = (float)j / slices * 2.0f * 3.14159f;
            float x = cosf(lng), z = sinf(lng);
            glNormal3f(x * r0, y0, z * r0);
            glVertex3f(x * r0 * 1.5f, y0 * 1.5f, z * r0 * 1.5f);
            glNormal3f(x * r1, y1, z * r1);
            glVertex3f(x * r1 * 1.5f, y1 * 1.5f, z * r1 * 1.5f);
        }
        glEnd();
    }
    glPopMatrix();

    /* Draw spout (cylinder pointing forward) */
    glPushMatrix();
    glTranslatef(0.0f, 0.3f, 1.2f);
    glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    glColor3f(0.7f, 0.2f, 0.2f);
    for (int i = 0; i < 4; i++) {
        float z0 = -0.8f + (float)i / 4 * 1.6f;
        float z1 = -0.8f + (float)(i + 1) / 4 * 1.6f;
        glBegin(GL_TRIANGLE_STRIP);
        for (int j = 0; j <= 8; j++) {
            float a = (float)j / 8 * 2.0f * 3.14159f;
            float r = 0.15f;
            float x = cosf(a) * r, y = sinf(a) * r;
            glNormal3f(x, y, 0);
            glVertex3f(x, y, z0);
            glNormal3f(x, y, 0);
            glVertex3f(x, y, z1);
        }
        glEnd();
    }
    glPopMatrix();

    /* Draw handle (curved cylinder on right side) */
    glPushMatrix();
    glTranslatef(1.5f, 0.5f, 0.0f);
    glRotatef(-30.0f, 0.0f, 0.0f, 1.0f);
    glColor3f(0.7f, 0.2f, 0.2f);
    for (int i = 0; i < 8; i++) {
        float a0 = (float)i / 8 * 3.14159f;
        float a1 = (float)(i + 1) / 8 * 3.14159f;
        float x0 = cosf(a0) * 0.8f, y0 = sinf(a0) * 0.8f;
        float x1 = cosf(a1) * 0.8f, y1 = sinf(a1) * 0.8f;
        glBegin(GL_TRIANGLE_STRIP);
        for (int j = 0; j <= 6; j++) {
            float b = (float)j / 6 * 2.0f * 3.14159f;
            float r = 0.12f;
            float cx = cosf(b) * r, cy = sinf(b) * r;
            glNormal3f(cx, cy, 0);
            glVertex3f(x0 + cx, y0 + cy, 0);
            glNormal3f(cx, cy, 0);
            glVertex3f(x1 + cx, y1 + cy, 0);
        }
        glEnd();
    }
    glPopMatrix();
}

/* ── TinyGL init ─────────────────────────────────────── */
static int gl_init(int w, int h)
{
    s_width  = w;
    s_height = h;

    /* 16-bit RGB565 mode: TinyGL renders directly to display backend buffer.
     * ZB_MODE_5R6G5B matches ST7735 native format, zero-copy. */
    s_zb = ZB_open(w, h, ZB_MODE_5R6G5B, s_display->get_buffer());
    if (!s_zb) { ESP_LOGE(TAG, "ZB_open failed"); return -1; }

    glInit(s_zb);
    glEnable(GL_DEPTH_TEST);

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)w / (float)h;
    glFrustum(-aspect, aspect, -1.0f, 1.0f, 0.5f, 20.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.1f, 0.1f, 0.2f, 0);
    glShadeModel(GL_SMOOTH);

    /* Turn on ST7735 display */

    /* Create a 256x256 checkerboard texture for testing.
     * TinyGL requires: GL_TEXTURE_2D, level=0, components=3, border=0,
     * format=GL_RGB, type=GL_UNSIGNED_BYTE, width=height=256.
     * Allocate in PSRAM to avoid DRAM overflow (256*256*3 = 192KB). */
    {
        GLubyte *tex = (GLubyte *)heap_caps_malloc(256 * 256 * 3, MALLOC_CAP_SPIRAM);
        if (!tex) {
            ESP_LOGE(TAG, "Texture alloc failed");
        } else {
            for (int y = 0; y < 256; y++) {
                for (int x = 0; x < 256; x++) {
                    GLubyte *p = &tex[(y * 256 + x) * 3];
                    /* Ceramic teapot: warm beige with gradient + speckles + highlight */
                    int br = 210 + (y * 30 / 256);
                    int bg = 180 + (y * 35 / 256);
                    int bb = 140 + (y * 40 / 256);
                    int n = ((x * 17 + y * 31) & 15) - 8;
                    p[0] = br + n > 255 ? 255 : (br + n < 0 ? 0 : br + n);
                    p[1] = bg + n > 255 ? 255 : (bg + n < 0 ? 0 : bg + n);
                    p[2] = bb + n > 255 ? 255 : (bb + n < 0 ? 0 : bb + n);
                    if (y < 32) { p[0]=p[0]*7/10; p[1]=p[1]*7/10; p[2]=p[2]*7/10; }
                    if (y > 100 && y < 120) { p[0]+=20; p[1]+=15; p[2]+=10; }
                }
            }
            glBindTexture(GL_TEXTURE_2D, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, tex);
            ESP_LOGI(TAG, "Texture uploaded: 256x256 RGB565");
            glEnable(GL_TEXTURE_2D);
            ESP_LOGI(TAG, "GL_TEXTURE_2D enabled");
            free(tex);
        }
    }

    ESP_LOGI(TAG, "TinyGL initialized: %dx%d", w, h);
    return 0;
}

/* ── Copy TinyGL framebuffer to display ──────────────────
 * 16-bit mode: TinyGL renders directly to display backend buffer.
 * Just flush. */
static void gl_flush_to_display(void)
{
    s_display->flush();
}

/* ── Render a single frame ───────────────────────────── */
static void render_frame(float angle_y)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0, 0, -2.5f);
    glRotatef(30, 1, 0, 0);
    glRotatef(angle_y, 0, 1, 0);

    /* Simple textured cube — 6 faces with explicit texture coords */
    float s = 1.0f;  /* half-size */

    glBegin(GL_QUADS);

    /* Front face (z = +s) */
    glTexCoord2f(0, 0); glVertex3f(-s, -s,  s);
    glTexCoord2f(1, 0); glVertex3f( s, -s,  s);
    glTexCoord2f(1, 1); glVertex3f( s,  s,  s);
    glTexCoord2f(0, 1); glVertex3f(-s,  s,  s);

    /* Back face (z = -s) */
    glTexCoord2f(0, 0); glVertex3f( s, -s, -s);
    glTexCoord2f(1, 0); glVertex3f(-s, -s, -s);
    glTexCoord2f(1, 1); glVertex3f(-s,  s, -s);
    glTexCoord2f(0, 1); glVertex3f( s,  s, -s);

    /* Top face (y = +s) */
    glTexCoord2f(0, 0); glVertex3f(-s,  s, -s);
    glTexCoord2f(1, 0); glVertex3f( s,  s, -s);
    glTexCoord2f(1, 1); glVertex3f( s,  s,  s);
    glTexCoord2f(0, 1); glVertex3f(-s,  s,  s);

    /* Bottom face (y = -s) */
    glTexCoord2f(0, 0); glVertex3f(-s, -s,  s);
    glTexCoord2f(1, 0); glVertex3f( s, -s,  s);
    glTexCoord2f(1, 1); glVertex3f( s, -s, -s);
    glTexCoord2f(0, 1); glVertex3f(-s, -s, -s);

    /* Right face (x = +s) */
    glTexCoord2f(0, 0); glVertex3f( s, -s,  s);
    glTexCoord2f(1, 0); glVertex3f( s, -s, -s);
    glTexCoord2f(1, 1); glVertex3f( s,  s, -s);
    glTexCoord2f(0, 1); glVertex3f( s,  s,  s);

    /* Left face (x = -s) */
    glTexCoord2f(0, 0); glVertex3f(-s, -s, -s);
    glTexCoord2f(1, 0); glVertex3f(-s, -s,  s);
    glTexCoord2f(1, 1); glVertex3f(-s,  s,  s);
    glTexCoord2f(0, 1); glVertex3f(-s,  s, -s);

    glEnd();

    gl_flush_to_display();
}

/* ── Benchmark task ──────────────────────────────────── */
void *tinygl_benchmark(void *arg)
{
    (void)arg;

    /* Use the ST7735 display backend */
    extern const display_backend_t st7735_display_backend;
    s_display = &st7735_display_backend;
    s_display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);

    if (gl_init(s_width, s_height) != 0) {
        ESP_LOGE(TAG, "gl_init failed");
        return NULL;
    }

    int frame_count = 0;
    float angle_y = 0;
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Teapot benchmark started — 5 second run");

    int64_t frame_start_us = 0;
    while (true) {
        frame_start_us = esp_timer_get_time();

        render_frame(angle_y);
        frame_count++;
        angle_y += 2.0f;

        /* Frame rate limit: target ~60 FPS (16667 us/frame) to reduce
         * tearing on ST7735 (TE pin not connected, no vsync possible).
         * Wait until 16ms has elapsed since frame start. */
        int64_t frame_elapsed = esp_timer_get_time() - frame_start_us;
        if (frame_elapsed < 16667) {
            vTaskDelay(pdMS_TO_TICKS((16667 - frame_elapsed) / 1000));
        }

        int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= 5000000) {
            float fps = (float)frame_count / ((float)elapsed_us / 1000000.0f);
            ESP_LOGI(TAG, "=== BENCHMARK RESULT ===");
            ESP_LOGI(TAG, "Frames: %d in %.2f sec = %.1f FPS",
                     frame_count, (float)elapsed_us / 1000000.0f, fps);
            ESP_LOGI(TAG, "=========================");
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }
}
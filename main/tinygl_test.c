/**
 * TinyGL benchmark + 3D demo on the Xiaomiao ESP32 handheld.
 *
 * Renders a rotating textured cube (or N cubes, or a physics-driven scene)
 * to the ST7735 via TinyGL's 16-bit RGB565 rasterizer. Features:
 *   - 4 compile-time textures (ceramic/checker/brick/grid), multi-bind
 *   - Directional light + material (Gouraud shading)
 *   - Gravity + ground/wall collision physics
 *   - 6-face skybox (rotation-only, depth-write off)
 *   - Backface culling + depth test
 *
 * The debug console (components/debug_console) exposes live REPL commands
 * to probe framebuffer/texture pixels, pause/resume, set cube count, etc.
 */

#include "GL/gl.h"
#include "zbuffer.h"
#include "zfeatures.h"
#include "zgl.h"            /* gl_get_context for diag */

#include "display_backend.h"
#ifndef TGL_EMU_BUILD
#include "hw_board.h"
#endif

#include "texture_ceramic.h"   /* compile-time generated, in build dir */
#include "texture_checker.h"
#include "texture_brick.h"
#include "texture_grid.h"
#include "tinygl_physics.h"

#ifndef TGL_EMU_BUILD
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
/* PC emulator shims — esp_compat.h provides ESP_LOGI and esp_timer_get_time */
#include "esp_compat.h"
#endif

#include <math.h>
#include <string.h>

static const char *TAG = "tinygl";

/* Pause flag for the debug console: when non-zero, the render loop skips
 * rendering+flush so debug commands can write the framebuffer and flush
 * without being clobbered. */
volatile int tinygl_render_paused = 0;

/* Log enable flag: when 0, the benchmark loop suppresses its 5s ESP_LOG
 * FPS report (so it doesn't pollute serial captures like `shot`). */
volatile int tinygl_log_enabled = 1;

/* Latest FPS measurement (updated every 5s by the render loop). */
volatile float tinygl_last_fps = 0.0f;

/* Physics mode: when non-zero, cubes are driven by the physics engine
 * (gravity + ground collision) instead of the static grid layout. */
volatile int tinygl_physics_mode = 0;

/* Cube count — adjustable via debug console for stress testing. */
volatile int tinygl_cube_count = 1;

/* Display backend — set by platform entry point before calling gl_init */
const display_backend_t *s_display = NULL;

/* ── Framebuffer + Zbuffer ───────────────────────────── */
static ZBuffer *s_zb  = NULL;
static int s_width  = 160;
static int s_height = 128;

/* Texture name → texture IDs (must match glBindTexture calls in draw_*). */
#define TEX_CERAMIC 1
#define TEX_CHECKER 2
#define TEX_BRICK   3
#define TEX_GRID    4

/* ── Spawn a falling cube into the physics engine (called by debug console). */
int tinygl_spawn_cube(void)
{
    static int idx = 0;
    float xs[] = {-2.0f, 0.0f, 2.0f, -1.0f, 1.0f};
    float x = xs[idx % 5];
    idx++;
    return physics_spawn(x, 4.0f, 0.0f, 0.8f);
}

/* ── Diagnose: print first pixels of framebuffer + zbuf[0] ──────────── */
static void diag_fb(const char *label)
{
    GLContext *c = gl_get_context();
    if (!c || !c->zb || !c->zb->pbuf) return;
    int y = c->zb->ysize / 2;
    uint16_t *fb = (uint16_t *)c->zb->pbuf;
    ESP_LOGI("diag", "%s: fb[0..7]=%04x %04x %04x %04x %04x %04x %04x %04x  zbuf[0]=%d",
             label,
             fb[y*160+0], fb[y*160+1], fb[y*160+2], fb[y*160+3],
             fb[y*160+4], fb[y*160+5], fb[y*160+6], fb[y*160+7],
             c->zb->zbuf[0]);
}

/* ── Render a single textured cube ────────────────────── */
static void draw_textured_cube(float x, float y, float z, float size,
                               float rx, float ry, GLuint tex_id)
{
    float s = size * 0.5f;
    glPushMatrix();
    glTranslatef(x, y, z);
    glRotatef(rx, 1, 0, 0);
    glRotatef(ry, 0, 1, 0);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glColor3f(1.0f, 1.0f, 1.0f);

    glBegin(GL_QUADS);
    /* Front  (+Z) */ glNormal3f(0,0,1);  glTexCoord2f(0,0); glVertex3f(-s,-s, s);
                     glNormal3f(0,0,1);  glTexCoord2f(1,0); glVertex3f( s,-s, s);
                     glNormal3f(0,0,1);  glTexCoord2f(1,1); glVertex3f( s, s, s);
                     glNormal3f(0,0,1);  glTexCoord2f(0,1); glVertex3f(-s, s, s);
    /* Back   (-Z) */ glNormal3f(0,0,-1); glTexCoord2f(0,0); glVertex3f( s,-s,-s);
                     glNormal3f(0,0,-1); glTexCoord2f(1,0); glVertex3f(-s,-s,-s);
                     glNormal3f(0,0,-1); glTexCoord2f(1,1); glVertex3f(-s, s,-s);
                     glNormal3f(0,0,-1); glTexCoord2f(0,1); glVertex3f( s, s,-s);
    /* Top    (+Y) */ glNormal3f(0,1,0);  glTexCoord2f(0,0); glVertex3f(-s, s, s);
                     glNormal3f(0,1,0);  glTexCoord2f(1,0); glVertex3f( s, s, s);
                     glNormal3f(0,1,0);  glTexCoord2f(1,1); glVertex3f( s, s,-s);
                     glNormal3f(0,1,0);  glTexCoord2f(0,1); glVertex3f(-s, s,-s);
    /* Bottom (-Y) */ glNormal3f(0,-1,0); glTexCoord2f(0,0); glVertex3f(-s,-s,-s);
                     glNormal3f(0,-1,0); glTexCoord2f(1,0); glVertex3f( s,-s,-s);
                     glNormal3f(0,-1,0); glTexCoord2f(1,1); glVertex3f( s,-s, s);
                     glNormal3f(0,-1,0); glTexCoord2f(0,1); glVertex3f(-s,-s, s);
    /* Right  (+X) */ glNormal3f(1,0,0);  glTexCoord2f(0,0); glVertex3f( s,-s, s);
                     glNormal3f(1,0,0);  glTexCoord2f(1,0); glVertex3f( s,-s,-s);
                     glNormal3f(1,0,0);  glTexCoord2f(1,1); glVertex3f( s, s,-s);
                     glNormal3f(1,0,0);  glTexCoord2f(0,1); glVertex3f( s, s, s);
    /* Left   (-X) */ glNormal3f(-1,0,0); glTexCoord2f(0,0); glVertex3f(-s,-s,-s);
                     glNormal3f(-1,0,0); glTexCoord2f(1,0); glVertex3f(-s,-s, s);
                     glNormal3f(-1,0,0); glTexCoord2f(1,1); glVertex3f(-s, s, s);
                     glNormal3f(-1,0,0); glTexCoord2f(0,1); glVertex3f(-s, s,-s);
    glEnd();
    glPopMatrix();
}

/* ── Skybox: large cube viewed from inside, 6 textured faces ──────────
 * Drawn first with depth-write off so scene geometry draws over it. */
static int tinygl_skybox_enabled = 1;
static void draw_skybox(void)
{
    if (!tinygl_skybox_enabled) return;
    float s = 15.0f;
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);      /* render all 6 inner faces */

    glBegin(GL_QUADS);
    /* +X */ glBindTexture(GL_TEXTURE_2D, TEX_CHECKER);
        glTexCoord2f(0,0); glVertex3f( s,-s,-s);
        glTexCoord2f(1,0); glVertex3f( s,-s, s);
        glTexCoord2f(1,1); glVertex3f( s, s, s);
        glTexCoord2f(0,1); glVertex3f( s, s,-s);
    /* -X */ glBindTexture(GL_TEXTURE_2D, TEX_BRICK);
        glTexCoord2f(0,0); glVertex3f(-s,-s, s);
        glTexCoord2f(1,0); glVertex3f(-s,-s,-s);
        glTexCoord2f(1,1); glVertex3f(-s, s,-s);
        glTexCoord2f(0,1); glVertex3f(-s, s, s);
    /* +Y */ glBindTexture(GL_TEXTURE_2D, TEX_GRID);
        glTexCoord2f(0,0); glVertex3f(-s, s, s);
        glTexCoord2f(1,0); glVertex3f( s, s, s);
        glTexCoord2f(1,1); glVertex3f( s, s,-s);
        glTexCoord2f(0,1); glVertex3f(-s, s,-s);
    /* -Y */ glBindTexture(GL_TEXTURE_2D, TEX_CERAMIC);
        glTexCoord2f(0,0); glVertex3f(-s,-s,-s);
        glTexCoord2f(1,0); glVertex3f( s,-s,-s);
        glTexCoord2f(1,1); glVertex3f( s,-s, s);
        glTexCoord2f(0,1); glVertex3f(-s,-s, s);
    /* +Z */ glBindTexture(GL_TEXTURE_2D, TEX_GRID);
        glTexCoord2f(0,0); glVertex3f(-s,-s, s);
        glTexCoord2f(1,0); glVertex3f( s,-s, s);
        glTexCoord2f(1,1); glVertex3f( s, s, s);
        glTexCoord2f(0,1); glVertex3f(-s, s, s);
    /* -Z */ glBindTexture(GL_TEXTURE_2D, TEX_BRICK);
        glTexCoord2f(0,0); glVertex3f( s,-s,-s);
        glTexCoord2f(1,0); glVertex3f(-s,-s,-s);
        glTexCoord2f(1,1); glVertex3f(-s, s,-s);
        glTexCoord2f(0,1); glVertex3f( s, s,-s);
    glEnd();

    /* restore state for scene geometry */
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glDepthMask(GL_TRUE);
}

/* ── Copy TinyGL framebuffer to display ────────────────── */
static void gl_flush_to_display(void)
{
    s_display->flush();
}

/* ── TinyGL init ─────────────────────────────────────── */
int gl_init(int w, int h)
{
    s_width  = w;
    s_height = h;

    s_zb = ZB_open(w, h, ZB_MODE_5R6G5B, s_display->get_buffer());
    if (!s_zb) { ESP_LOGE(TAG, "ZB_open failed"); return -1; }

    glInit(s_zb);
    glEnable(GL_DEPTH_TEST);

    /* Backface culling — all cube faces use CCW winding with outward normals. */
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)w / (float)h;
    glFrustum(-aspect, aspect, -1.0f, 1.0f, 1.0f, 30.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(0.1f, 0.1f, 0.2f, 0);
    glShadeModel(GL_SMOOTH);

    /* ── Lighting ── one directional light + ambient. */
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat ambient[]  = { 0.2f, 0.2f, 0.2f, 1.0f };
    GLfloat diffuse[]  = { 0.9f, 0.9f, 0.85f, 1.0f };
    GLfloat light_pos[] = { 5.0f, 8.0f, 5.0f, 0.0f };  /* w=0 → directional */
    glLightfv(GL_LIGHT0, GL_AMBIENT,  ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  diffuse);
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    GLfloat mat_amb[] = { 0.3f, 0.3f, 0.3f, 1.0f };
    GLfloat mat_dif[] = { 0.8f, 0.8f, 0.8f, 1.0f };
    glMaterialfv(GL_FRONT, GL_AMBIENT, mat_amb);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_dif);
    ESP_LOGI(TAG, "Lighting enabled (LIGHT0 directional + ambient)");

    /* ── Textures ── 4 compile-time textures uploaded to flash-backed IDs. */
    {
        struct { GLuint id; const GLvoid *data; const char *name; } texs[] = {
            { TEX_CERAMIC, texture_ceramic_data, "ceramic" },
            { TEX_CHECKER, texture_checker_data, "checker" },
            { TEX_BRICK,   texture_brick_data,   "brick"   },
            { TEX_GRID,    texture_grid_data,    "grid"    },
        };
        for (int i = 0; i < 4; i++) {
            glBindTexture(GL_TEXTURE_2D, texs[i].id);
            glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0,
                         GL_RGB, GL_UNSIGNED_BYTE, texs[i].data);
            ESP_LOGI(TAG, "Texture %d uploaded: %s", texs[i].id, texs[i].name);
        }
        glEnable(GL_TEXTURE_2D);
        ESP_LOGI(TAG, "GL_TEXTURE_2D enabled (4 textures bound)");
    }

    diag_fb("after_init");
    ESP_LOGI(TAG, "TinyGL initialized: %dx%d", w, h);
    return 0;
}

/* ── Render a single frame ───────────────────────────── */
void render_frame(float angle_y)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* ── Skybox ── centered on camera (rotation only, no translation). */
    glLoadIdentity();
    glRotatef(25, 1, 0, 0);
    glRotatef(angle_y, 0, 1, 0);
    draw_skybox();

    /* ── Scene ── translate camera back, then rotate. */
    glLoadIdentity();
    glTranslatef(0, 0, -3.5f);
    glRotatef(25, 1, 0, 0);
    glRotatef(angle_y, 0, 1, 0);

    int n = tinygl_cube_count;
    if (n < 1) n = 1;
    static const GLuint cube_tex[4] = { TEX_CERAMIC, TEX_CHECKER, TEX_BRICK, TEX_GRID };

    if (tinygl_physics_mode) {
        physics_step(1.0f / 60.0f);
        int drawn = 0;
        for (int i = 0; i < MAX_BODIES && drawn < n; i++) {
            body_t *b = physics_get(i);
            if (!b || !b->active) continue;
            draw_textured_cube(b->x, b->y, b->z, b->hs * 2.0f,
                               0.0f, 0.0f, cube_tex[drawn % 4]);
            drawn++;
        }
    } else {
        int per_row = 1;
        while (per_row * per_row < n) per_row++;
        for (int i = 0; i < n; i++) {
            int row = i / per_row;
            int col = i % per_row;
            float spacing = 2.5f;
            float ox = (col - (per_row - 1) * 0.5f) * spacing;
            float oy = (row - (per_row - 1) * 0.5f) * spacing;
            draw_textured_cube(ox, oy, 0.0f, 1.6f,
                               angle_y * (0.5f + i * 0.07f),
                               angle_y * (0.7f + i * 0.11f),
                               cube_tex[i % 4]);
        }
    }

    gl_flush_to_display();
}

/* ── Benchmark task ──────────────────────────────────── */
#ifndef TGL_EMU_BUILD
void *tinygl_benchmark(void *arg)
{
    (void)arg;

    extern const display_backend_t st7735_display_backend;
    s_display = &st7735_display_backend;
    s_display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);

    if (gl_init(s_width, s_height) != 0) {
        ESP_LOGE(TAG, "gl_init failed");
        return NULL;
    }

    physics_init();

    int frame_count = 0;
    float angle_y = 0;
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "TinyGL benchmark started");

    int64_t frame_start_us = 0;
    while (true) {
        frame_start_us = esp_timer_get_time();

        if (!tinygl_render_paused) {
            render_frame(angle_y);
            frame_count++;
            angle_y += 2.0f;
        }

        int64_t frame_elapsed = esp_timer_get_time() - frame_start_us;
        if (frame_elapsed < 16667) {
            vTaskDelay(pdMS_TO_TICKS((16667 - frame_elapsed) / 1000));
        } else {
            vTaskDelay(1);
        }

        int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= 5000000) {
            float fps = (float)frame_count / ((float)elapsed_us / 1000000.0f);
            tinygl_last_fps = fps;
            if (tinygl_log_enabled) {
                ESP_LOGI(TAG, "=== BENCHMARK RESULT ===");
                ESP_LOGI(TAG, "Frames: %d in %.2f sec = %.1f FPS",
                         frame_count, (float)elapsed_us / 1000000.0f, fps);
                ESP_LOGI(TAG, "=========================");
            }
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }
}
#endif /* !TGL_EMU_BUILD */

/**
 * TinyGL rendering pipeline for the Xiaomiao ESP32 handheld.
 *
 * Thin abstraction over TinyGL's fixed-function OpenGL 1.x subset:
 *   - gl_init()   — Z-buffer, textures, lighting one-time setup
 *   - render_frame() — skybox → scene geometry → flush
 *   - draw_textured_cube() / draw_skybox() — geometry helpers
 *
 * All GL state lives in TinyGL's global context (gl_ctx). This file is
 * a convenience wrapper, not a full scene-graph engine.
 */

#include "tinygl_pipeline.h"

#include "GL/gl.h"
#include "zbuffer.h"
#include "zfeatures.h"
#include "zgl.h"

#include "display_backend.h"

#include "texture_ceramic.h"
#include "texture_checker.h"
#include "texture_brick.h"
#include "texture_grid.h"

#ifndef TGL_EMU_BUILD
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include "esp_compat.h"
#endif

#include <math.h>
#include <string.h>

static const char *TAG = "pipeline";

/* ── Global state ───────────────────────────────────────────────────────── */

/* Display backend — set by platform entry point before calling gl_init */
extern const display_backend_t st7735_display_backend;
const display_backend_t *s_display = NULL;

ZBuffer *s_zb  = NULL;
static int s_width  = 160;
static int s_height = 128;

/* Runtime controls (cross-task volatile — written by debug console) */
volatile int tinygl_render_paused = 0;
volatile int tinygl_log_enabled  = 1;
volatile float tinygl_last_fps   = 0.0f;
volatile int tinygl_physics_mode = 0;
volatile int tinygl_cube_count   = 1;
int tinygl_skybox_enabled        = 1;

/* ── Skybox ──────────────────────────────────────────────────────────────── */

static void draw_skybox_face(float x0, float y0, float z0,
                             float x1, float y1, float z1,
                             float x2, float y2, float z2,
                             float x3, float y3, float z3)
{
    /* Each face subdivided into 4×4 quads to avoid perspective distortion
     * on large triangles. */
    int sub = 4;
    float dxdu = (x1 - x0) / sub, dydu = (y1 - y0) / sub, dzdu = (z1 - z0) / sub;
    float dxdv = (x3 - x0) / sub, dydv = (y3 - y0) / sub, dzdv = (z3 - z0) / sub;

    for (int v = 0; v < sub; v++) {
        float v0 = (float)v / sub, v1 = (float)(v + 1) / sub;
        glBegin(GL_QUAD_STRIP);
        for (int u = 0; u <= sub; u++) {
            float uf = (float)u / sub;
            float tu = 1.0f - uf;
            /* row v0 */
            glTexCoord2f(tu, v0);
            glVertex3f(x0 + dxdu*u + dxdv*v0,
                       y0 + dydu*u + dydv*v0,
                       z0 + dzdu*u + dzdv*v0);
            /* row v1 */
            glTexCoord2f(tu, v1);
            glVertex3f(x0 + dxdu*u + dxdv*v1,
                       y0 + dydu*u + dydv*v1,
                       z0 + dzdu*u + dzdv*v1);
        }
        glEnd();
    }
}

void draw_skybox(void)
{
    if (!tinygl_skybox_enabled) return;

    float s = 15.0f;  /* far enough to be behind all scene geometry */
    glPushMatrix();
    glBindTexture(GL_TEXTURE_2D, TEX_GRID);
    glColor3f(1.0f, 1.0f, 1.0f);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    /* 6 faces: +X, -X, +Y, -Y, +Z, -Z */
    draw_skybox_face( s,-s, s,  s, s, s,  s, s,-s,  s,-s,-s);  /* +X */
    draw_skybox_face(-s,-s,-s, -s, s,-s, -s, s, s, -s,-s, s);  /* -X */
    draw_skybox_face(-s, s, s,  s, s, s,  s, s,-s, -s, s,-s);  /* +Y */
    draw_skybox_face(-s,-s,-s,  s,-s,-s,  s,-s, s, -s,-s, s);  /* -Y */
    draw_skybox_face(-s,-s, s,  s,-s, s,  s, s, s, -s, s, s);  /* +Z */
    draw_skybox_face( s,-s,-s, -s,-s,-s, -s, s,-s,  s, s,-s);  /* -Z */

    /* restore state for scene geometry */
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glDepthMask(GL_TRUE);
    glPopMatrix();
}

/* ── Textured cube ──────────────────────────────────────────────────────── */

void draw_textured_cube(float x, float y, float z, float size,
                        float rx, float ry, unsigned int tex_id)
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

/* ── Flush to display ───────────────────────────────────────────────────── */

static void gl_flush_to_display(void)
{
    s_display->flush();
}

/* ── GL init ────────────────────────────────────────────────────────────── */

int gl_init(int w, int h)
{
    s_width  = w;
    s_height = h;

    /* Use the ST7735 display backend */
    s_display = &st7735_display_backend;
    s_display->init(w, h, PIXEL_FORMAT_RGB565_SWAP);

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
    glFrustum(-aspect, aspect, -1.0f, 1.0f, 0.5f, 20.0f);

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
                         GL_RGB, GL_UNSIGNED_BYTE,
                         (GLvoid *)texs[i].data);
            ESP_LOGI(TAG, "Texture %d uploaded: %s (256x256, flash)", texs[i].id, texs[i].name);
        }
    }

    glBindTexture(GL_TEXTURE_2D, TEX_CERAMIC);
    glEnable(GL_TEXTURE_2D);

    ESP_LOGI(TAG, "TinyGL pipeline initialized: %dx%d", w, h);
    return 0;
}

/* ── Render one frame ───────────────────────────────────────────────────── */

#include "tinygl_physics.h"

/* ── Spawn a falling cube into the physics engine (called by debug console). */
int tinygl_spawn_cube(void)
{
    static int idx = 0;
    float xs[] = {-2.0f, 0.0f, 2.0f, -1.0f, 1.0f};
    float x = xs[idx % 5];
    idx++;
    return physics_spawn(x, 4.0f, 0.0f, 0.8f);
}

void render_frame(float angle_y)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* ── Skybox ── centred on camera (rotation only, no translation). */
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
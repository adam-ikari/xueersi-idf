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
ZBuffer *s_zb  = NULL;
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

/* ── Render a reflective cube (environment map reflection) ──
 * For each vertex, compute reflection vector R = I - 2*(N·I)*N
 * where I = eye-to-vertex direction, N = vertex normal.
 * Map R to sphere-map texture coords: s=(Rx+1)/2, t=(Rz+1)/2.
 * Uses the skybox texture (TEX_GRID) as the environment map. */
static void draw_reflective_cube(float cx, float cy, float cz, float size,
                                  float rx, float ry)
{
    float hs = size * 0.5f;
    glPushMatrix();
    glTranslatef(cx, cy, cz);
    glRotatef(rx, 1, 0, 0);
    glRotatef(ry, 0, 1, 0);
    glBindTexture(GL_TEXTURE_2D, TEX_GRID);
    glColor3f(1.0f, 1.0f, 1.0f);

    /* Camera position in world space (before model transform) */
    float eye_x = 0, eye_y = 0, eye_z = 5.0f;

    /* 6 faces: normal + 4 vertices, same layout as draw_textured_cube */
    static const struct { float nx,ny,nz; } norms[6] = {
        {0,0,1}, {0,0,-1}, {0,1,0}, {0,-1,0}, {1,0,0}, {-1,0,0}
    };

    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        float nx = norms[f].nx, ny = norms[f].ny, nz = norms[f].nz;
        glNormal3f(nx, ny, nz);

        /* 4 corner vertices for this face */
        float verts[4][3];
        if (f == 0) { /* Front +Z */
            verts[0][0]=-hs; verts[0][1]=-hs; verts[0][2]= hs;
            verts[1][0]= hs; verts[1][1]=-hs; verts[1][2]= hs;
            verts[2][0]= hs; verts[2][1]= hs; verts[2][2]= hs;
            verts[3][0]=-hs; verts[3][1]= hs; verts[3][2]= hs;
        } else if (f == 1) { /* Back -Z */
            verts[0][0]= hs; verts[0][1]=-hs; verts[0][2]=-hs;
            verts[1][0]=-hs; verts[1][1]=-hs; verts[1][2]=-hs;
            verts[2][0]=-hs; verts[2][1]= hs; verts[2][2]=-hs;
            verts[3][0]= hs; verts[3][1]= hs; verts[3][2]=-hs;
        } else if (f == 2) { /* Top +Y */
            verts[0][0]=-hs; verts[0][1]= hs; verts[0][2]= hs;
            verts[1][0]= hs; verts[1][1]= hs; verts[1][2]= hs;
            verts[2][0]= hs; verts[2][1]= hs; verts[2][2]=-hs;
            verts[3][0]=-hs; verts[3][1]= hs; verts[3][2]=-hs;
        } else if (f == 3) { /* Bottom -Y */
            verts[0][0]=-hs; verts[0][1]=-hs; verts[0][2]=-hs;
            verts[1][0]= hs; verts[1][1]=-hs; verts[1][2]=-hs;
            verts[2][0]= hs; verts[2][1]=-hs; verts[2][2]= hs;
            verts[3][0]=-hs; verts[3][1]=-hs; verts[3][2]= hs;
        } else if (f == 4) { /* Right +X */
            verts[0][0]= hs; verts[0][1]=-hs; verts[0][2]= hs;
            verts[1][0]= hs; verts[1][1]=-hs; verts[1][2]=-hs;
            verts[2][0]= hs; verts[2][1]= hs; verts[2][2]=-hs;
            verts[3][0]= hs; verts[3][1]= hs; verts[3][2]= hs;
        } else { /* Left -X */
            verts[0][0]=-hs; verts[0][1]=-hs; verts[0][2]=-hs;
            verts[1][0]=-hs; verts[1][1]=-hs; verts[1][2]= hs;
            verts[2][0]=-hs; verts[2][1]= hs; verts[2][2]= hs;
            verts[3][0]=-hs; verts[3][1]= hs; verts[3][2]=-hs;
        }

        for (int v = 0; v < 4; v++) {
            float vx = verts[v][0], vy = verts[v][1], vz = verts[v][2];
            /* View direction from eye to vertex */
            float ix = vx - eye_x, iy = vy - eye_y, iz = vz - eye_z;
            float il = sqrtf(ix*ix + iy*iy + iz*iz);
            if (il > 0.001f) { ix /= il; iy /= il; iz /= il; }
            /* Reflection: R = I - 2*(N·I)*N */
            float ndoti = nx*ix + ny*iy + nz*iz;
            float rr_x = ix - 2.0f*ndoti*nx;
            float rr_z = iz - 2.0f*ndoti*nz;
            /* Sphere map: s=(Rx+1)/2, t=(Rz+1)/2 */
            float tc_s = (rr_x + 1.0f) * 0.5f;
            float tc_t = (rr_z + 1.0f) * 0.5f;
            if (tc_s < 0) tc_s = 0;
            if (tc_s > 1) tc_s = 1;
            if (tc_t < 0) tc_t = 0;
            if (tc_t > 1) tc_t = 1;
            glTexCoord2f(tc_s, tc_t);
            glVertex3f(vx, vy, vz);
        }
    }
    glEnd();
    glPopMatrix();
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

/* ── Skybox: large cube viewed from inside, subdivided faces ──────────
 * Drawn first with depth-write off so scene geometry draws over it.
 * Each face is subdivided into NxN quads to reduce texture distortion. */
static int tinygl_skybox_enabled = 1;
static void draw_skybox_face(float x0, float y0, float z0,
                             float ax, float ay, float az,
                             float bx, float by, float bz,
                             int subdiv, GLuint tex_id)
{
    glBindTexture(GL_TEXTURE_2D, tex_id);
    float step = 1.0f / (float)subdiv;
    for (int i = 0; i < subdiv; i++) {
        for (int j = 0; j < subdiv; j++) {
            float u0 = i * step, u1 = (i + 1) * step;
            float v0 = j * step, v1 = (j + 1) * step;
            glBegin(GL_QUADS);
            glTexCoord2f(u0, v0); glVertex3f(x0 + ax*u0 + bx*v0, y0 + ay*u0 + by*v0, z0 + az*u0 + bz*v0);
            glTexCoord2f(u1, v0); glVertex3f(x0 + ax*u1 + bx*v0, y0 + ay*u1 + by*v0, z0 + az*u1 + bz*v0);
            glTexCoord2f(u1, v1); glVertex3f(x0 + ax*u1 + bx*v1, y0 + ay*u1 + by*v1, z0 + az*u1 + bz*v1);
            glTexCoord2f(u0, v1); glVertex3f(x0 + ax*u0 + bx*v1, y0 + ay*u0 + by*v1, z0 + az*u0 + bz*v1);
            glEnd();
        }
    }
}

static void draw_skybox(void)
{
    if (!tinygl_skybox_enabled) return;
    float s = 15.0f;
    int subdiv = 4;  /* 4x4 sub-quads per face */
    glDepthMask(GL_FALSE);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);

    GLContext* c = gl_get_context();
    c->use_affine_texture = 0;  /* perspective correction works fine on small quads */

    /* +X face: x=s, yz plane, normal +X */
    draw_skybox_face( s, -s, -s,  0, s*2, 0,  0, 0, s*2,  subdiv, TEX_CHECKER);
    /* -X face: x=-s, yz plane, normal -X */
    draw_skybox_face(-s, -s,  s,  0, s*2, 0,  0, 0,-s*2,  subdiv, TEX_BRICK);
    /* +Y face: y=s, xz plane, normal +Y */
    draw_skybox_face(-s,  s,  s,  s*2, 0, 0,  0, 0,-s*2,  subdiv, TEX_GRID);
    /* -Y face: y=-s, xz plane, normal -Y */
    draw_skybox_face(-s, -s, -s,  s*2, 0, 0,  0, 0, s*2,  subdiv, TEX_CERAMIC);
    /* +Z face: z=s, xy plane, normal +Z */
    draw_skybox_face(-s, -s,  s,  s*2, 0, 0,  0, s*2, 0,  subdiv, TEX_GRID);
    /* -Z face: z=-s, xy plane, normal -Z */
    draw_skybox_face( s, -s, -s, -s*2, 0, 0,  0, s*2, 0,  subdiv, TEX_BRICK);

    /* restore state for scene geometry */
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glDepthMask(GL_TRUE);
}

/* ── Ordered dithering (Bayer 4×4) for 16-bit color ────
 * Simulates higher color depth by spreading quantization error across
 * adjacent pixels. The 4×4 Bayer matrix adds a perceptually uniform
 * pattern that's far less visible than 16-bit color banding.
 * Cost: ~2μs per 160×128 frame (negligible). */
static const int8_t s_bayer4[16] = {
     0, -8,  2, -6,
    -4,  4, -2,  6,
     3, -5,  1, -7,
    -1,  7, -3,  5,
};

static GLuint dither_callback(GLint x, GLint y, GLuint pixel, GLushort z)
{
    (void)z;
    /* Un-swap to get logical RGB565 */
    uint16_t w = (uint16_t)pixel;
#if TGL_PIXEL_BYTE_SWAP
    w = (uint16_t)((w << 8) | (w >> 8));
#endif
    /* Extract 8-bit channels */
    int r = (w >> 11) & 0x1f;  r = (r << 3) | (r >> 2);  /* 5→8 bit */
    int g = (w >>  5) & 0x3f;  g = (g << 2) | (g >> 4);  /* 6→8 bit */
    int b =  w        & 0x1f;  b = (b << 3) | (b >> 2);  /* 5→8 bit */
    /* Apply Bayer offset */
    int idx = ((y & 3) << 2) | (x & 3);
    int d = s_bayer4[idx];
    r += d; g += d; b += d;
    /* Clamp */
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    /* Back to RGB565 */
    w = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
#if TGL_PIXEL_BYTE_SWAP
    w = (uint16_t)((w << 8) | (w >> 8));
#endif
    return (GLuint)w;
}

/* ── Copy TinyGL framebuffer to display ────────────────── */
static void gl_flush_to_display(void)
{
    /* Apply ordered dithering to reduce 16-bit color banding */
    glPostProcess(dither_callback);
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

/* ── Render task (pinned to core 1) ──────────────────── */
#ifndef TGL_EMU_BUILD
static void tinygl_render_task(void *arg)
{
    (void)arg;

    int frame_count = 0;
    float angle_y = 0;
    int64_t start_us = esp_timer_get_time();
    int64_t frame_start_us;

    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());

    while (1) {
        frame_start_us = esp_timer_get_time();

        if (!tinygl_render_paused) {
            /* Wait for previous frame's DMA to complete before writing pbuf.
             * This prevents rendering from corrupting the buffer being
             * transmitted — the root cause of the "missing polygon" flicker. */
            s_display->wait_dma();

            render_frame(angle_y);
            frame_count++;
            angle_y += 2.0f;
        }

        /* Frame rate limiter: target 30 FPS for stable, tear-free display.
         * The ST7735 has no TE pin, so exceeding 30 FPS causes visible
         * micro-tearing. Lock at 30 FPS for buttery smooth visuals. */
        int64_t frame_elapsed = esp_timer_get_time() - frame_start_us;
        int64_t target_frame_us = 33333;  /* 30 FPS = 33.3ms */
        if (frame_elapsed < target_frame_us) {
            int64_t remain_us = target_frame_us - frame_elapsed;
            if (remain_us > 1000) {
                vTaskDelay(pdMS_TO_TICKS(remain_us / 1000));
            }
            /* Fine-spin remaining <1ms for precision */
            while (esp_timer_get_time() - frame_start_us < target_frame_us) {
                /* busy wait, but very short (<1ms) */
            }
        } else {
            vTaskDelay(1);
        }

        int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= 5000000) {
            float fps = (float)frame_count / ((float)elapsed_us / 1000000.0f);
            tinygl_last_fps = fps;
            if (tinygl_log_enabled) {
                ESP_LOGI(TAG, "=== BENCHMARK RESULT ===");
                ESP_LOGI(TAG, "Frames: %d in %.2f sec = %.1f FPS (core %d)",
                         frame_count, (float)elapsed_us / 1000000.0f, fps, xPortGetCoreID());
                ESP_LOGI(TAG, "=========================");
            }
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }
}
#endif

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
    ESP_LOGI(TAG, "TinyGL benchmark starting on core 1");

    /* Create render task pinned to core 1 */
    xTaskCreatePinnedToCore(
        tinygl_render_task,
        "tinygl_render",
        8192,   /* stack size (words) -- larger for FPU context */
        NULL,
        configMAX_PRIORITIES - 1,
        NULL,
        1       /* core 1 */
    );

    /* Core 0: idle -- available for debug console, I2C, etc. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return NULL;
}
#endif /* !TGL_EMU_BUILD */

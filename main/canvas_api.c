/*
 * Canvas 2D Host API for WAMR — Implementation
 *
 * Provides the complete Canvas 2D drawing API for WASM games on the
 * Xiaomiao handheld. Renders into the hardware framebuffer (hw_fb)
 * which is then flushed to the ST7735 display.
 *
 * Functions use simple software rendering: primitives draw directly
 * into the RGB565 framebuffer, and canvas_end_frame() flushes the
 * buffer to the physical display.
 */

#include "canvas_api.h"
#include "hw_fb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "canvas_api";

/* ------------------------------------------------------------------ */
/* Canvas state (singleton)                                            */
/* ------------------------------------------------------------------ */


/* Canvas state (opaque to callers) */
/* Path command types */
enum {
    PATH_MOVE_TO = 0,
    PATH_LINE_TO,
    PATH_CLOSE,
};
typedef struct {
    /* Framebuffer state */
    uint16_t *fb;
    int fb_width;
    int fb_height;

    /* Style */
    uint8_t fill_r, fill_g, fill_b, fill_a;
    uint8_t stroke_r, stroke_g, stroke_b, stroke_a;
    float global_alpha;
    float line_width;

    /* Text */
    char font_name[MAX_FONT_NAME];
    int text_align;

    /* Transform stack */
    float translate_x, translate_y;
    float scale_x, scale_y;
    float rotate_rad;
    int transform_stack_top;
    float saved_transforms[MAX_TRANSFORM_STACK][5];

    /* Path state */
    struct {
        int cmd;
        float x;
        float y;
    } path_points[MAX_PATH_POINTS];
    int path_count;
    float path_current_x, path_current_y;
    float path_start_x, path_start_y;
    int path_has_current;

    /* Image tracking */
    struct {
        ximg_t *img;
        int id;            /* WASM-facing image ID; 0 = free slot */
    } image_slots[MAX_IMAGES];
    int next_image_id;

    /* Initialized flag */
    bool initialized;
} canvas_state_t;
static canvas_state_t s_canvas;

/* Convenience macro for WAMR signature registration */
#define CANVAS_SYMBOL(name, sig) \
    { #name, (void *)host_ ## name, sig, NULL }

/* Forward declarations of all host functions */
static int32_t host_canvas_begin_frame(wasm_exec_env_t exec_env);
static int32_t host_canvas_end_frame(wasm_exec_env_t exec_env);
static int32_t host_canvas_get_width(wasm_exec_env_t exec_env);
static int32_t host_canvas_get_height(wasm_exec_env_t exec_env);

static int32_t host_canvas_set_fill_style(wasm_exec_env_t exec_env, int32_t r, int32_t g, int32_t b, int32_t a);
static int32_t host_canvas_set_stroke_style(wasm_exec_env_t exec_env, int32_t r, int32_t g, int32_t b, int32_t a);
static int32_t host_canvas_set_global_alpha(wasm_exec_env_t exec_env, float alpha);
static int32_t host_canvas_set_line_width(wasm_exec_env_t exec_env, float width);

static int32_t host_canvas_fill_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h);
static int32_t host_canvas_clear_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h);
static int32_t host_canvas_stroke_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h);

static int32_t host_canvas_begin_path(wasm_exec_env_t exec_env);
static int32_t host_canvas_move_to(wasm_exec_env_t exec_env, float x, float y);
static int32_t host_canvas_line_to(wasm_exec_env_t exec_env, float x, float y);
static int32_t host_canvas_stroke(wasm_exec_env_t exec_env);
static int32_t host_canvas_fill(wasm_exec_env_t exec_env);
static int32_t host_canvas_close_path(wasm_exec_env_t exec_env);
static int32_t host_canvas_arc(wasm_exec_env_t exec_env, float x, float y, float r, float start_angle, float end_angle);
static int32_t host_canvas_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h);

static int32_t host_canvas_set_font(wasm_exec_env_t exec_env, int32_t font_ptr);
static int32_t host_canvas_fill_text(wasm_exec_env_t exec_env, int32_t text_ptr, float x, float y);
static int32_t host_canvas_set_text_align(wasm_exec_env_t exec_env, int32_t align);
static int32_t host_canvas_measure_text(wasm_exec_env_t exec_env, int32_t text_ptr);

static int32_t host_canvas_load_image(wasm_exec_env_t exec_env, int32_t path_ptr);
static int32_t host_canvas_image_get_width(wasm_exec_env_t exec_env, int32_t img_id);
static int32_t host_canvas_image_get_height(wasm_exec_env_t exec_env, int32_t img_id);
static int32_t host_canvas_draw_image(wasm_exec_env_t exec_env, int32_t img_id, float x, float y);
static int32_t host_canvas_draw_image_scaled(wasm_exec_env_t exec_env, int32_t img_id, float x, float y, float w, float h);
static int32_t host_canvas_draw_image_frame(wasm_exec_env_t exec_env, int32_t img_id, float x, float y,
                                           int32_t fx, int32_t fy, int32_t fw, int32_t fh);
static int32_t host_canvas_draw_image_frame_scaled(wasm_exec_env_t exec_env, int32_t img_id,
                                                  float x, float y, float w, float h,
                                                  int32_t fx, int32_t fy, int32_t fw, int32_t fh);
static int32_t host_canvas_unload_image(wasm_exec_env_t exec_env, int32_t img_id);

static int32_t host_canvas_put_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y, int32_t r, int32_t g, int32_t b, int32_t a);
static int32_t host_canvas_get_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y);

static int32_t host_canvas_translate(wasm_exec_env_t exec_env, float dx, float dy);
static int32_t host_canvas_rotate(wasm_exec_env_t exec_env, float angle);
static int32_t host_canvas_scale(wasm_exec_env_t exec_env, float sx, float sy);
static int32_t host_canvas_save(wasm_exec_env_t exec_env);
static int32_t host_canvas_restore(wasm_exec_env_t exec_env);
static int32_t host_canvas_reset_transform(wasm_exec_env_t exec_env);

/* ------------------------------------------------------------------ */
/* Color conversion helpers                                            */
/* ------------------------------------------------------------------ */

static uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void rgb565_to_rgba(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    *r = (uint8_t)(((c >> 11) & 0x1F) << 3);
    *g = (uint8_t)(((c >> 5) & 0x3F) << 2);
    *b = (uint8_t)((c & 0x1F) << 3);
    *a = 255;
}

static uint16_t blend_pixel(uint16_t dst, uint16_t src, uint8_t src_a)
{
    if (src_a >= 254) {
        return src;
    }
    if (src_a == 0) {
        return dst;
    }

    uint8_t dr, dg, db, da;
    uint8_t sr, sg, sb, sa;
    rgb565_to_rgba(dst, &dr, &dg, &db, &da);
    rgb565_to_rgba(src, &sr, &sg, &sb, &sa);

    float alpha = src_a / 255.0f;
    float inv_alpha = 1.0f - alpha;

    uint8_t rr = (uint8_t)(sr * alpha + dr * inv_alpha);
    uint8_t rg = (uint8_t)(sg * alpha + dg * inv_alpha);
    uint8_t rb = (uint8_t)(sb * alpha + db * inv_alpha);

    return rgb_to_565(rr, rg, rb);
}

/* ------------------------------------------------------------------ */
/* Clipping helpers                                                    */
/* ------------------------------------------------------------------ */

static bool clip_rect(int *x, int *y, int *w, int *h)
{
    if (*x >= CANVAS_WIDTH || *y >= CANVAS_HEIGHT || *x + *w <= 0 || *y + *h <= 0) {
        return false;
    }
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > CANVAS_WIDTH) {
        *w = CANVAS_WIDTH - *x;
    }
    if (*y + *h > CANVAS_HEIGHT) {
        *h = CANVAS_HEIGHT - *y;
    }
    return *w > 0 && *h > 0;
}

/* ------------------------------------------------------------------ */
/* Transform helpers                                                   */
/* ------------------------------------------------------------------ */

static void apply_transform(float *out_x, float *out_y)
{
    float x = *out_x;
    float y = *out_y;

    /* Scale */
    x *= s_canvas.scale_x;
    y *= s_canvas.scale_y;

    /* Rotate */
    if (s_canvas.rotate_rad != 0.0f) {
        float cos_r = cosf(s_canvas.rotate_rad);
        float sin_r = sinf(s_canvas.rotate_rad);
        float nx = x * cos_r - y * sin_r;
        float ny = x * sin_r + y * cos_r;
        x = nx;
        y = ny;
    }

    /* Translate */
    x += s_canvas.translate_x;
    y += s_canvas.translate_y;

    *out_x = x;
    *out_y = y;
}

/* ------------------------------------------------------------------ */
/* Line drawing (Bresenham)                                            */
/* ------------------------------------------------------------------ */

static void draw_line_low(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int yi = 1;
    if (dy < 0) {
        yi = -1;
        dy = -dy;
    }
    int D = 2 * dy - dx;
    int y = y0;
    for (int x = x0; x <= x1; x++) {
        if (x >= 0 && x < CANVAS_WIDTH && y >= 0 && y < CANVAS_HEIGHT) {
            int idx = y * CANVAS_WIDTH + x;
            s_canvas.fb[idx] = blend_pixel(s_canvas.fb[idx], color, alpha);
        }
        if (D > 0) {
            y += yi;
            D -= 2 * dx;
        }
        D += 2 * dy;
    }
}

static void draw_line_high(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xi = 1;
    if (dx < 0) {
        xi = -1;
        dx = -dx;
    }
    int D = 2 * dx - dy;
    int x = x0;
    for (int y = y0; y <= y1; y++) {
        if (x >= 0 && x < CANVAS_WIDTH && y >= 0 && y < CANVAS_HEIGHT) {
            int idx = y * CANVAS_WIDTH + x;
            s_canvas.fb[idx] = blend_pixel(s_canvas.fb[idx], color, alpha);
        }
        if (D > 0) {
            x += xi;
            D -= 2 * dy;
        }
        D += 2 * dx;
    }
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha)
{
    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) {
            draw_line_low(x1, y1, x0, y0, color, alpha);
        } else {
            draw_line_low(x0, y0, x1, y1, color, alpha);
        }
    } else {
        if (y0 > y1) {
            draw_line_high(x1, y1, x0, y0, color, alpha);
        } else {
            draw_line_high(x0, y0, x1, y1, color, alpha);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Thick line drawing                                                  */
/* ------------------------------------------------------------------ */

static void draw_thick_line(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha, float width)
{
    int half_w = (int)(width / 2.0f);
    if (half_w < 1) {
        draw_line(x0, y0, x1, y1, color, alpha);
        return;
    }

    /* Draw multiple offset lines for thickness */
    float dx = (float)(x1 - x0);
    float dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) {
        return;
    }
    float nx = -dy / len;
    float ny = dx / len;

    for (int i = -half_w; i <= half_w; i++) {
        int ox = (int)(nx * i);
        int oy = (int)(ny * i);
        draw_line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color, alpha);
    }
}

/* ------------------------------------------------------------------ */
/* Flood fill helper for path fill                                     */
/* ------------------------------------------------------------------ */

static void draw_filled_polygon(float *verts_x, float *verts_y, int nverts,
                                 uint16_t color, uint8_t alpha)
{
    if (nverts < 3) return;

    /* Find bounds */
    int min_y = CANVAS_HEIGHT, max_y = 0;
    for (int i = 0; i < nverts; i++) {
        int vy = (int)verts_y[i];
        if (vy < min_y) min_y = vy;
        if (vy > max_y) max_y = vy;
    }
    if (min_y < 0) min_y = 0;
    if (max_y >= CANVAS_HEIGHT) max_y = CANVAS_HEIGHT - 1;
    if (min_y > max_y) return;

    /* Scanline fill using active edge table (simple non-AA version) */
    for (int y = min_y; y <= max_y; y++) {
        float intersections[256];
        int nint = 0;

        for (int i = 0; i < nverts; i++) {
            int j = (i + 1) % nverts;
            float y0f = verts_y[i];
            float y1f = verts_y[j];
            float x0f = verts_x[i];
            float x1f = verts_x[j];

            if (y0f > y1f) {
                float tmp;
                tmp = y0f; y0f = y1f; y1f = tmp;
                tmp = x0f; x0f = x1f; x1f = tmp;
            }

            if ((float)y >= y0f && (float)y < y1f) {
                float t = ((float)y - y0f) / (y1f - y0f);
                float ix = x0f + t * (x1f - x0f);
                if (nint < 256) {
                    intersections[nint++] = ix;
                }
            }
        }

        /* Sort intersections */
        for (int k = 0; k < nint - 1; k++) {
            for (int l = k + 1; l < nint; l++) {
                if (intersections[k] > intersections[l]) {
                    float tmp = intersections[k];
                    intersections[k] = intersections[l];
                    intersections[l] = tmp;
                }
            }
        }

        /* Fill spans */
        for (int k = 0; k + 1 < nint; k += 2) {
            int x_start = (int)intersections[k];
            int x_end = (int)intersections[k + 1];
            if (x_start < 0) x_start = 0;
            if (x_end >= CANVAS_WIDTH) x_end = CANVAS_WIDTH - 1;
            for (int x = x_start; x <= x_end; x++) {
                int idx = y * CANVAS_WIDTH + x;
                s_canvas.fb[idx] = blend_pixel(s_canvas.fb[idx], color, alpha);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Arc helper                                                          */
/* ------------------------------------------------------------------ */

static void add_arc_points(float cx, float cy, float r,
                            float start_angle, float end_angle,
                            float *out_x, float *out_y, int *npoints)
{
    int segments = (int)(fabsf(end_angle - start_angle) * 8.0f / 3.14159f);
    if (segments < 4) segments = 4;
    if (segments > 64) segments = 64;

    float angle_step = (end_angle - start_angle) / segments;
    for (int i = 0; i <= segments && *npoints < MAX_PATH_POINTS - 1; i++) {
        float a = start_angle + i * angle_step;
        out_x[*npoints] = cx + r * cosf(a);
        out_y[*npoints] = cy + r * sinf(a);
        (*npoints)++;
    }
}

/* ------------------------------------------------------------------ */
/* Host function implementations                                       */
/* ------------------------------------------------------------------ */

/* --- Frame Control --- */

static int32_t host_canvas_begin_frame(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;
    /* Clear framebuffer to black */
    memset(s_canvas.fb, 0, CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(uint16_t));
    return 0;
}

static int32_t host_canvas_end_frame(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;
    /* Flush the framebuffer to the display */
    hw_fb_flush();
    return 0;
}

static int32_t host_canvas_get_width(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return CANVAS_WIDTH;
}

static int32_t host_canvas_get_height(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    return CANVAS_HEIGHT;
}

/* --- Style --- */

static int32_t host_canvas_set_fill_style(wasm_exec_env_t exec_env, int32_t r, int32_t g, int32_t b, int32_t a)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.fill_r = (uint8_t)(r & 0xFF);
    s_canvas.fill_g = (uint8_t)(g & 0xFF);
    s_canvas.fill_b = (uint8_t)(b & 0xFF);
    s_canvas.fill_a = (uint8_t)(a & 0xFF);
    return 0;
}

static int32_t host_canvas_set_stroke_style(wasm_exec_env_t exec_env, int32_t r, int32_t g, int32_t b, int32_t a)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.stroke_r = (uint8_t)(r & 0xFF);
    s_canvas.stroke_g = (uint8_t)(g & 0xFF);
    s_canvas.stroke_b = (uint8_t)(b & 0xFF);
    s_canvas.stroke_a = (uint8_t)(a & 0xFF);
    return 0;
}

static int32_t host_canvas_set_global_alpha(wasm_exec_env_t exec_env, float alpha)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.global_alpha = alpha;
    if (s_canvas.global_alpha < 0.0f) s_canvas.global_alpha = 0.0f;
    if (s_canvas.global_alpha > 1.0f) s_canvas.global_alpha = 1.0f;
    return 0;
}

static int32_t host_canvas_set_line_width(wasm_exec_env_t exec_env, float width)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.line_width = width;
    if (s_canvas.line_width < 0.5f) s_canvas.line_width = 0.5f;
    return 0;
}

/* --- Rectangles --- */

static int32_t host_canvas_fill_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    /* Transform both position and extent */
    float tx, ty, tx2, ty2;
    tx = x; ty = y;
    apply_transform(&tx, &ty);
    tx2 = x + w; ty2 = y + h;
    apply_transform(&tx2, &ty2);

    int ix = (int)tx;
    int iy = (int)ty;
    int iw = abs((int)(tx2 - tx));
    int ih = abs((int)(ty2 - ty));

    if (!clip_rect(&ix, &iy, &iw, &ih)) return 0;

    uint8_t sr = s_canvas.fill_r;
    uint8_t sg = s_canvas.fill_g;
    uint8_t sb = s_canvas.fill_b;
    uint8_t sa = s_canvas.fill_a;
    uint8_t fa = (uint8_t)(sa * s_canvas.global_alpha);

    /* Fast path: fully opaque fill — use hw_fb_fill_rect directly */
    if (fa == 0xFF) {
        hw_fb_fill_rect(ix, iy, iw, ih, rgb_to_565(sr, sg, sb));
        return 0;
    }

    uint16_t color = rgb_to_565(sr, sg, sb);

    for (int row = iy; row < iy + ih; row++) {
        for (int col = ix; col < ix + iw; col++) {
            int idx = row * CANVAS_WIDTH + col;
            s_canvas.fb[idx] = blend_pixel(s_canvas.fb[idx], color, fa);
        }
    }
    return 0;
}

static int32_t host_canvas_clear_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    /* Transform both position and extent */
    float tx, ty, tx2, ty2;
    tx = x; ty = y;
    apply_transform(&tx, &ty);
    tx2 = x + w; ty2 = y + h;
    apply_transform(&tx2, &ty2);

    int ix = (int)tx;
    int iy = (int)ty;
    int iw = abs((int)(tx2 - tx));
    int ih = abs((int)(ty2 - ty));

    if (!clip_rect(&ix, &iy, &iw, &ih)) return 0;

    for (int row = iy; row < iy + ih; row++) {
        memset(&s_canvas.fb[row * CANVAS_WIDTH + ix], 0, iw * sizeof(uint16_t));
    }
    return 0;
}

static int32_t host_canvas_stroke_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    /* Transform both corners */
    float x1, y1, x2, y2;
    x1 = x; y1 = y;
    apply_transform(&x1, &y1);
    x2 = x + w; y2 = y + h;
    apply_transform(&x2, &y2);

    uint16_t color = rgb_to_565(s_canvas.stroke_r, s_canvas.stroke_g, s_canvas.stroke_b);
    uint8_t alpha = (uint8_t)(s_canvas.stroke_a * s_canvas.global_alpha);
    float lw = s_canvas.line_width;

    draw_thick_line((int)x1, (int)y1, (int)x2, (int)y1, color, alpha, lw);      /* top */
    draw_thick_line((int)x1, (int)y2, (int)x2, (int)y2, color, alpha, lw);      /* bottom */
    draw_thick_line((int)x1, (int)y1, (int)x1, (int)y2, color, alpha, lw);      /* left */
    draw_thick_line((int)x2, (int)y1, (int)x2, (int)y2, color, alpha, lw);      /* right */
    return 0;
}

/* --- Paths --- */

static int32_t host_canvas_begin_path(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.path_count = 0;
    s_canvas.path_has_current = false;
    return 0;
}

static int32_t host_canvas_move_to(wasm_exec_env_t exec_env, float x, float y)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    if (s_canvas.path_count >= MAX_PATH_POINTS) return -1;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_MOVE_TO;
    s_canvas.path_points[s_canvas.path_count].x = x;
    s_canvas.path_points[s_canvas.path_count].y = y;
    s_canvas.path_count++;

    s_canvas.path_current_x = x;
    s_canvas.path_current_y = y;
    s_canvas.path_start_x = x;
    s_canvas.path_start_y = y;
    s_canvas.path_has_current = true;
    return 0;
}

static int32_t host_canvas_line_to(wasm_exec_env_t exec_env, float x, float y)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    if (s_canvas.path_count >= MAX_PATH_POINTS) return -1;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_LINE_TO;
    s_canvas.path_points[s_canvas.path_count].x = x;
    s_canvas.path_points[s_canvas.path_count].y = y;
    s_canvas.path_count++;

    s_canvas.path_current_x = x;
    s_canvas.path_current_y = y;
    return 0;
}

static int32_t host_canvas_close_path(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    if (s_canvas.path_count >= MAX_PATH_POINTS) return -1;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_CLOSE;
    s_canvas.path_points[s_canvas.path_count].x = s_canvas.path_start_x;
    s_canvas.path_points[s_canvas.path_count].y = s_canvas.path_start_y;
    s_canvas.path_count++;

    s_canvas.path_current_x = s_canvas.path_start_x;
    s_canvas.path_current_y = s_canvas.path_start_y;
    return 0;
}

static int32_t host_canvas_stroke(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;
    if (s_canvas.path_count < 2) return 0;

    uint16_t color = rgb_to_565(s_canvas.stroke_r, s_canvas.stroke_g, s_canvas.stroke_b);
    uint8_t alpha = (uint8_t)(s_canvas.stroke_a * s_canvas.global_alpha);

    float prev_x = 0, prev_y = 0;
    bool has_prev = false;

    for (int i = 0; i < s_canvas.path_count; i++) {
        float px = s_canvas.path_points[i].x;
        float py = s_canvas.path_points[i].y;

        if (s_canvas.path_points[i].cmd == PATH_MOVE_TO) {
            has_prev = false;
            prev_x = px;
            prev_y = py;
            has_prev = true;
        } else if (s_canvas.path_points[i].cmd == PATH_LINE_TO) {
            if (has_prev) {
                float tx1 = prev_x, ty1 = prev_y;
                float tx2 = px, ty2 = py;
                apply_transform(&tx1, &ty1);
                apply_transform(&tx2, &ty2);
                draw_thick_line((int)tx1, (int)ty1, (int)tx2, (int)ty2,
                                color, alpha, s_canvas.line_width);
            }
            prev_x = px;
            prev_y = py;
            has_prev = true;
        } else if (s_canvas.path_points[i].cmd == PATH_CLOSE) {
            if (has_prev) {
                float tx1 = prev_x, ty1 = prev_y;
                float tx2 = px, ty2 = py;
                apply_transform(&tx1, &ty1);
                apply_transform(&tx2, &ty2);
                draw_thick_line((int)tx1, (int)ty1, (int)tx2, (int)ty2,
                                color, alpha, s_canvas.line_width);
            }
            prev_x = px;
            prev_y = py;
            has_prev = true;
        }
    }
    return 0;
}

static int32_t host_canvas_fill(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;
    if (s_canvas.path_count < 3) return 0;

    /* Extract vertices for polygon fill */
    float verts_x[256];
    float verts_y[256];
    int nverts = 0;

    for (int i = 0; i < s_canvas.path_count && nverts < 256; i++) {
        float px = s_canvas.path_points[i].x;
        float py = s_canvas.path_points[i].y;

        if (s_canvas.path_points[i].cmd == PATH_CLOSE) {
            /* close path: skip duplicating, the polygon already has all edges */
            continue;
        }

        if (s_canvas.path_points[i].cmd == PATH_MOVE_TO && nverts > 0) {
            /* Start new subpath — for now just fill the first one,
               then reset for simplicity. But we'll just continue
               and accumulate all points for a single fill. */
        }

        apply_transform(&px, &py);
        verts_x[nverts] = px;
        verts_y[nverts] = py;
        nverts++;
    }

    if (nverts < 3) return 0;

    uint16_t color = rgb_to_565(s_canvas.fill_r, s_canvas.fill_g, s_canvas.fill_b);
    uint8_t alpha = (uint8_t)(s_canvas.fill_a * s_canvas.global_alpha);

    draw_filled_polygon(verts_x, verts_y, nverts, color, alpha);
    return 0;
}

static int32_t host_canvas_arc(wasm_exec_env_t exec_env, float x, float y, float r,
                              float start_angle, float end_angle)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;

    float arc_x[64], arc_y[64];
    int narc = 0;

    add_arc_points(x, y, r, start_angle, end_angle, arc_x, arc_y, &narc);

    for (int i = 0; i < narc && s_canvas.path_count < MAX_PATH_POINTS; i++) {
        if (i == 0) {
            s_canvas.path_points[s_canvas.path_count].cmd = PATH_MOVE_TO;
        } else {
            s_canvas.path_points[s_canvas.path_count].cmd = PATH_LINE_TO;
        }
        s_canvas.path_points[s_canvas.path_count].x = arc_x[i];
        s_canvas.path_points[s_canvas.path_count].y = arc_y[i];
        s_canvas.path_count++;
    }

    if (narc > 0) {
        s_canvas.path_current_x = arc_x[narc - 1];
        s_canvas.path_current_y = arc_y[narc - 1];
        if (!s_canvas.path_has_current) {
            s_canvas.path_start_x = arc_x[0];
            s_canvas.path_start_y = arc_y[0];
            s_canvas.path_has_current = true;
        }
    }
    return 0;
}

static int32_t host_canvas_rect(wasm_exec_env_t exec_env, float x, float y, float w, float h)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    if (s_canvas.path_count + 5 > MAX_PATH_POINTS) return -1;

    float x2 = x + w;
    float y2 = y + h;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_MOVE_TO;
    s_canvas.path_points[s_canvas.path_count].x = x;
    s_canvas.path_points[s_canvas.path_count].y = y;
    s_canvas.path_count++;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_LINE_TO;
    s_canvas.path_points[s_canvas.path_count].x = x2;
    s_canvas.path_points[s_canvas.path_count].y = y;
    s_canvas.path_count++;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_LINE_TO;
    s_canvas.path_points[s_canvas.path_count].x = x2;
    s_canvas.path_points[s_canvas.path_count].y = y2;
    s_canvas.path_count++;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_LINE_TO;
    s_canvas.path_points[s_canvas.path_count].x = x;
    s_canvas.path_points[s_canvas.path_count].y = y2;
    s_canvas.path_count++;

    s_canvas.path_points[s_canvas.path_count].cmd = PATH_CLOSE;
    s_canvas.path_points[s_canvas.path_count].x = x;
    s_canvas.path_points[s_canvas.path_count].y = y;
    s_canvas.path_count++;

    s_canvas.path_current_x = x;
    s_canvas.path_current_y = y2;
    s_canvas.path_start_x = x;
    s_canvas.path_start_y = y;
    s_canvas.path_has_current = true;
    return 0;
}

/* --- Text --- */

static int32_t host_canvas_set_font(wasm_exec_env_t exec_env, int32_t font_ptr)
{
    if (!s_canvas.initialized || !font_ptr) return -1;
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    if (!module_inst) return -1;

    const char *font_str = (const char *)wasm_runtime_addr_app_to_native(module_inst, (uint32_t)font_ptr);
    if (!font_str) return -1;

    strncpy(s_canvas.font_name, font_str, sizeof(s_canvas.font_name) - 1);
    s_canvas.font_name[sizeof(s_canvas.font_name) - 1] = '\0';
    return 0;
}

/* Built-in 5x7 monospace bitmap font (ASCII 32-126) */
static const uint8_t s_font_5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x32}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x41,0x41,0x7F,0x00,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x08,0x54,0x54,0x54,0x3C}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x00,0x7F,0x10,0x28,0x44}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x08,0x2A,0x1C,0x08}, /* ~ */
};

#define FONT_WIDTH  5
#define FONT_HEIGHT 7

static void draw_char(int x, int y, char c, uint16_t color, uint8_t alpha)
{
    if (c < 32 || c > 126) return;
    int idx = c - 32;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (s_font_5x7[idx][col] & (1 << row)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < CANVAS_WIDTH && py >= 0 && py < CANVAS_HEIGHT) {
                    s_canvas.fb[py * CANVAS_WIDTH + px] = blend_pixel(
                        s_canvas.fb[py * CANVAS_WIDTH + px], color, alpha);
                }
            }
        }
    }
}

static int text_width_pixels(const char *text)
{
    return (int)strlen(text) * (FONT_WIDTH + 1);
}

static int32_t host_canvas_fill_text(wasm_exec_env_t exec_env, int32_t text_ptr, float x, float y)
{
    if (!s_canvas.initialized || !s_canvas.fb || !text_ptr) return -1;
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    if (!module_inst) return -1;

    const char *text = (const char *)wasm_runtime_addr_app_to_native(module_inst, (uint32_t)text_ptr);
    if (!text) return -1;

    uint16_t color = rgb_to_565(s_canvas.fill_r, s_canvas.fill_g, s_canvas.fill_b);
    uint8_t alpha = (uint8_t)(s_canvas.fill_a * s_canvas.global_alpha);

    int len = (int)strlen(text);
    int text_w = len * (FONT_WIDTH + 1);
    int start_x = (int)x;

    if (s_canvas.text_align == 1) {
        /* Center */
        start_x = (int)x - text_w / 2;
    } else if (s_canvas.text_align == 2) {
        /* Right */
        start_x = (int)x - text_w;
    }

    for (int i = 0; text[i] != '\0'; i++) {
        draw_char(start_x + i * (FONT_WIDTH + 1), (int)y, text[i], color, alpha);
    }

    return 0;
}

static int32_t host_canvas_set_text_align(wasm_exec_env_t exec_env, int32_t align)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.text_align = (int)align;
    if (s_canvas.text_align < 0) s_canvas.text_align = 0;
    if (s_canvas.text_align > 2) s_canvas.text_align = 2;
    return 0;
}

static int32_t host_canvas_measure_text(wasm_exec_env_t exec_env, int32_t text_ptr)
{
    if (!s_canvas.initialized || !text_ptr) return -1;
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    if (!module_inst) return -1;

    const char *text = (const char *)wasm_runtime_addr_app_to_native(module_inst, (uint32_t)text_ptr);
    if (!text) return -1;

    return text_width_pixels(text);
}

/* --- Image --- */

static int32_t host_canvas_load_image(wasm_exec_env_t exec_env, int32_t path_ptr)
{
    if (!s_canvas.initialized || !path_ptr) return -1;
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    if (!module_inst) return -1;

    const char *path = (const char *)wasm_runtime_addr_app_to_native(module_inst, (uint32_t)path_ptr);
    if (!path) return -1;

    /* Open the file from SD card */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open image: %s", path);
        return -1;
    }

    /* Read header: 4 bytes "XIMG" magic, 2 bytes width, 2 bytes height, 1 byte format */
    uint8_t header[9];
    if (fread(header, 1, 9, f) != 9) {
        fclose(f);
        return -1;
    }

    if (memcmp(header, "XIMG", 4) != 0) {
        /* Try loading as raw 160x128 RGB565 file */
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        /* Attempt to auto-detect: if file size matches FB size, treat as raw RGB565 */
        if (fsize == CANVAS_WIDTH * CANVAS_HEIGHT * 2) {
            ximg_t *img = (ximg_t *)malloc(sizeof(ximg_t));
            if (!img) { fclose(f); return -1; }
            img->width = CANVAS_WIDTH;
            img->height = CANVAS_HEIGHT;
            img->is_indexed = false;
            img->palette = NULL;
            img->ref_count = 1;
            img->pixels = (uint8_t *)malloc((size_t)fsize);
            if (!img->pixels) {
                free(img);
                fclose(f);
                return -1;
            }
            fread(img->pixels, 1, (size_t)fsize, f);
            fclose(f);

            int id = s_canvas.next_image_id++;
            for (int i = 0; i < MAX_IMAGES; i++) {
                if (s_canvas.image_slots[i].img == NULL) {
                    s_canvas.image_slots[i].img = img;
                    s_canvas.image_slots[i].id = id;
                    ESP_LOGI(TAG, "Loaded raw RGB565 image id=%d (%dx%d) from %s",
                             id, img->width, img->height, path);
                    return id;
                }
            }
            /* No free slot */
            free(img->pixels);
            free(img);
            return -1;
        }

        fclose(f);
        ESP_LOGW(TAG, "Unknown image format: %s", path);
        return -1;
    }

    int w = (int)header[4] | ((int)header[5] << 8);
    int h = (int)header[6] | ((int)header[7] << 8);
    bool is_indexed = (header[8] == 1);

    if (w <= 0 || w > CANVAS_WIDTH * 2 || h <= 0 || h > CANVAS_HEIGHT * 2) {
        fclose(f);
        ESP_LOGW(TAG, "Invalid image dimensions: %dx%d", w, h);
        return -1;
    }

    ximg_t *img = (ximg_t *)malloc(sizeof(ximg_t));
    if (!img) { fclose(f); return -1; }
    img->width = w;
    img->height = h;
    img->is_indexed = is_indexed;
    img->ref_count = 1;

    if (is_indexed) {
        /* Indexed8: next 256*3 bytes = RGB332 palette, then w*h bytes of indices */
        img->palette = (uint8_t *)malloc(768);
        if (!img->palette) { free(img); fclose(f); return -1; }
        if (fread(img->palette, 1, 768, f) != 768) {
            free(img->palette);
            free(img);
            fclose(f);
            return -1;
        }
        img->pixels = (uint8_t *)malloc((size_t)(w * h));
        if (!img->pixels) {
            free(img->palette);
            free(img);
            fclose(f);
            return -1;
        }
        if (fread(img->pixels, 1, (size_t)(w * h), f) != (size_t)(w * h)) {
            free(img->pixels);
            free(img->palette);
            free(img);
            fclose(f);
            return -1;
        }
    } else {
        /* RGB565: w*h*2 bytes of pixel data */
        img->palette = NULL;
        img->pixels = (uint8_t *)malloc((size_t)(w * h * 2));
        if (!img->pixels) { free(img); fclose(f); return -1; }
        if (fread(img->pixels, 1, (size_t)(w * h * 2), f) != (size_t)(w * h * 2)) {
            free(img->pixels);
            free(img);
            fclose(f);
            return -1;
        }
    }

    fclose(f);

    int id = s_canvas.next_image_id++;
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (s_canvas.image_slots[i].img == NULL) {
            s_canvas.image_slots[i].img = img;
            s_canvas.image_slots[i].id = id;
            ESP_LOGI(TAG, "Loaded image id=%d (%dx%d, %s) from %s",
                     id, w, h, is_indexed ? "Indexed8" : "RGB565", path);
            return id;
        }
    }

    /* No free slot */
    free(img->pixels);
    if (img->palette) free(img->palette);
    free(img);
    return -1;
}

static int32_t host_canvas_image_get_width(wasm_exec_env_t exec_env, int32_t img_id)
{
    (void)exec_env;
    if (!s_canvas.initialized || img_id <= 0) return -1;

    for (int i = 0; i < MAX_IMAGES; i++) {
        if (s_canvas.image_slots[i].img != NULL && s_canvas.image_slots[i].id == img_id) {
            return s_canvas.image_slots[i].img->width;
        }
    }
    return -1;
}

static int32_t host_canvas_image_get_height(wasm_exec_env_t exec_env, int32_t img_id)
{
    (void)exec_env;
    if (!s_canvas.initialized || img_id <= 0) return -1;

    for (int i = 0; i < MAX_IMAGES; i++) {
        if (s_canvas.image_slots[i].img != NULL && s_canvas.image_slots[i].id == img_id) {
            return s_canvas.image_slots[i].img->height;
        }
    }
    return -1;
}

static ximg_t *find_image(int img_id)
{
    for (int i = 0; i < MAX_IMAGES; i++) {
        if (s_canvas.image_slots[i].img != NULL && s_canvas.image_slots[i].id == img_id) {
            return s_canvas.image_slots[i].img;
        }
    }
    return NULL;
}

static void draw_image_rgb565(ximg_t *img, int dx, int dy, int dw, int dh,
                               int sx, int sy, int sw, int sh)
{
    uint8_t alpha = (uint8_t)(255 * s_canvas.global_alpha);

    float scale_x = (float)dw / sw;
    float scale_y = (float)dh / sh;

    for (int row = 0; row < dh; row++) {
        for (int col = 0; col < dw; col++) {
            int src_x = sx + (int)(col / scale_x);
            int src_y = sy + (int)(row / scale_y);
            if (src_x >= img->width) src_x = img->width - 1;
            if (src_y >= img->height) src_y = img->height - 1;

            int dst_x = dx + col;
            int dst_y = dy + row;

            if (dst_x < 0 || dst_x >= CANVAS_WIDTH || dst_y < 0 || dst_y >= CANVAS_HEIGHT) continue;

            uint16_t pixel = *(uint16_t *)&img->pixels[(src_y * img->width + src_x) * 2];
            int idx = dst_y * CANVAS_WIDTH + dst_x;
            s_canvas.fb[idx] = blend_pixel(s_canvas.fb[idx], pixel, alpha);
        }
    }
}

static void draw_image_indexed8(ximg_t *img, int dx, int dy, int dw, int dh,
                                  int sx, int sy, int sw, int sh)
{
    uint8_t alpha = (uint8_t)(255 * s_canvas.global_alpha);

    float scale_x = (float)dw / sw;
    float scale_y = (float)dh / sh;

    for (int row = 0; row < dh; row++) {
        for (int col = 0; col < dw; col++) {
            int src_x = sx + (int)(col / scale_x);
            int src_y = sy + (int)(row / scale_y);
            if (src_x >= img->width) src_x = img->width - 1;
            if (src_y >= img->height) src_y = img->height - 1;

            int dst_x = dx + col;
            int dst_y = dy + row;

            if (dst_x < 0 || dst_x >= CANVAS_WIDTH || dst_y < 0 || dst_y >= CANVAS_HEIGHT) continue;

            uint8_t idx_val = img->pixels[src_y * img->width + src_x];
            /* Palette entry: 3 bytes per entry (RGB332-ish, stored as R,G,B raw) */
            uint8_t pr = img->palette[idx_val * 3];
            uint8_t pg = img->palette[idx_val * 3 + 1];
            uint8_t pb = img->palette[idx_val * 3 + 2];

            uint16_t pixel = rgb_to_565(pr, pg, pb);
            int fidx = dst_y * CANVAS_WIDTH + dst_x;
            s_canvas.fb[fidx] = blend_pixel(s_canvas.fb[fidx], pixel, alpha);
        }
    }
}

static int32_t host_canvas_draw_image(wasm_exec_env_t exec_env, int32_t img_id, float x, float y)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    ximg_t *img = find_image(img_id);
    if (!img) return -1;

    /* Transform both position and extent */
    float tx, ty, tx2, ty2;
    tx = x; ty = y;
    apply_transform(&tx, &ty);
    tx2 = x + img->width; ty2 = y + img->height;
    apply_transform(&tx2, &ty2);

    int dx = (int)tx;
    int dy = (int)ty;
    int dw = abs((int)(tx2 - tx));
    int dh = abs((int)(ty2 - ty));
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    if (img->is_indexed) {
        draw_image_indexed8(img, dx, dy, dw, dh,
                            0, 0, img->width, img->height);
    } else {
        draw_image_rgb565(img, dx, dy, dw, dh,
                          0, 0, img->width, img->height);
    }
    return 0;
}

static int32_t host_canvas_draw_image_scaled(wasm_exec_env_t exec_env, int32_t img_id,
                                            float x, float y, float w, float h)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    ximg_t *img = find_image(img_id);
    if (!img) return -1;

    /* Transform both position and extent */
    float tx, ty, tx2, ty2;
    tx = x; ty = y;
    apply_transform(&tx, &ty);
    tx2 = x + w; ty2 = y + h;
    apply_transform(&tx2, &ty2);

    int dx = (int)tx;
    int dy = (int)ty;
    int dw = abs((int)(tx2 - tx));
    int dh = abs((int)(ty2 - ty));
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    if (img->is_indexed) {
        draw_image_indexed8(img, dx, dy, dw, dh, 0, 0, img->width, img->height);
    } else {
        draw_image_rgb565(img, dx, dy, dw, dh, 0, 0, img->width, img->height);
    }
    return 0;
}

static int32_t host_canvas_draw_image_frame(wasm_exec_env_t exec_env, int32_t img_id, float x, float y,
                                           int32_t fx, int32_t fy, int32_t fw, int32_t fh)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    ximg_t *img = find_image(img_id);
    if (!img) return -1;

    /* Transform both position and extent */
    float tx, ty, tx2, ty2;
    tx = x; ty = y;
    apply_transform(&tx, &ty);
    tx2 = x + fw; ty2 = y + fh;
    apply_transform(&tx2, &ty2);

    int dx = (int)tx;
    int dy = (int)ty;
    int dw = abs((int)(tx2 - tx));
    int dh = abs((int)(ty2 - ty));
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;

    if (img->is_indexed) {
        draw_image_indexed8(img, dx, dy, dw, dh, (int)fx, (int)fy, (int)fw, (int)fh);
    } else {
        draw_image_rgb565(img, dx, dy, dw, dh, (int)fx, (int)fy, (int)fw, (int)fh);
    }
    return 0;
}

static int32_t host_canvas_draw_image_frame_scaled(wasm_exec_env_t exec_env, int32_t img_id,
                                                  float x, float y, float w, float h,
                                                  int32_t fx, int32_t fy, int32_t fw, int32_t fh)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;

    ximg_t *img = find_image(img_id);
    if (!img) return -1;

    /* Transform both position and extent */
    float tx, ty, tx2, ty2;
    tx = x; ty = y;
    apply_transform(&tx, &ty);
    tx2 = x + w; ty2 = y + h;
    apply_transform(&tx2, &ty2);

    int dx = (int)tx;
    int dy = (int)ty;
    int dw = abs((int)(tx2 - tx));
    int dh = abs((int)(ty2 - ty));
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;

    if (img->is_indexed) {
        draw_image_indexed8(img, dx, dy, dw, dh, (int)fx, (int)fy, (int)fw, (int)fh);
    } else {
        draw_image_rgb565(img, dx, dy, dw, dh, (int)fx, (int)fy, (int)fw, (int)fh);
    }
    return 0;
}

static int32_t host_canvas_unload_image(wasm_exec_env_t exec_env, int32_t img_id)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;

    for (int i = 0; i < MAX_IMAGES; i++) {
        if (s_canvas.image_slots[i].img != NULL && s_canvas.image_slots[i].id == img_id) {
            ximg_t *img = s_canvas.image_slots[i].img;
            img->ref_count--;
            if (img->ref_count <= 0) {
                free(img->pixels);
                if (img->palette) free(img->palette);
                free(img);
            }
            s_canvas.image_slots[i].img = NULL;
            s_canvas.image_slots[i].id = 0;
            return 0;
        }
    }
    return -1;
}

/* --- Pixel Operations --- */

static int32_t host_canvas_put_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y,
                                    int32_t r, int32_t g, int32_t b, int32_t a)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) return 0;

    uint16_t color = rgb_to_565((uint8_t)r, (uint8_t)g, (uint8_t)b);
    uint8_t alpha = (uint8_t)(a * s_canvas.global_alpha);
    int idx = y * CANVAS_WIDTH + x;
    s_canvas.fb[idx] = blend_pixel(s_canvas.fb[idx], color, alpha);
    return 0;
}

static int32_t host_canvas_get_pixel(wasm_exec_env_t exec_env, int32_t x, int32_t y)
{
    (void)exec_env;
    if (!s_canvas.initialized || !s_canvas.fb) return -1;
    if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) return 0;

    return (int32_t)s_canvas.fb[y * CANVAS_WIDTH + x];
}

/* --- Transform --- */

static int32_t host_canvas_translate(wasm_exec_env_t exec_env, float dx, float dy)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.translate_x += dx;
    s_canvas.translate_y += dy;
    return 0;
}

static int32_t host_canvas_rotate(wasm_exec_env_t exec_env, float angle)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.rotate_rad += angle;
    return 0;
}

static int32_t host_canvas_scale(wasm_exec_env_t exec_env, float sx, float sy)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.scale_x *= sx;
    s_canvas.scale_y *= sy;
    return 0;
}

static int32_t host_canvas_save(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    if (s_canvas.transform_stack_top >= MAX_TRANSFORM_STACK) return -1;

    int top = s_canvas.transform_stack_top;
    s_canvas.saved_transforms[top][0] = s_canvas.translate_x;
    s_canvas.saved_transforms[top][1] = s_canvas.translate_y;
    s_canvas.saved_transforms[top][2] = s_canvas.scale_x;
    s_canvas.saved_transforms[top][3] = s_canvas.scale_y;
    s_canvas.saved_transforms[top][4] = s_canvas.rotate_rad;
    s_canvas.transform_stack_top++;
    return 0;
}

static int32_t host_canvas_restore(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    if (s_canvas.transform_stack_top <= 0) return -1;

    s_canvas.transform_stack_top--;
    int top = s_canvas.transform_stack_top;
    s_canvas.translate_x = s_canvas.saved_transforms[top][0];
    s_canvas.translate_y = s_canvas.saved_transforms[top][1];
    s_canvas.scale_x = s_canvas.saved_transforms[top][2];
    s_canvas.scale_y = s_canvas.saved_transforms[top][3];
    s_canvas.rotate_rad = s_canvas.saved_transforms[top][4];
    return 0;
}

static int32_t host_canvas_reset_transform(wasm_exec_env_t exec_env)
{
    (void)exec_env;
    if (!s_canvas.initialized) return -1;
    s_canvas.translate_x = 0.0f;
    s_canvas.translate_y = 0.0f;
    s_canvas.scale_x = 1.0f;
    s_canvas.scale_y = 1.0f;
    s_canvas.rotate_rad = 0.0f;
    s_canvas.transform_stack_top = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* NativeSymbol table                                                  */
/* ------------------------------------------------------------------ */

static NativeSymbol s_canvas_symbols[] = {
    /* Frame Control */
    CANVAS_SYMBOL(canvas_begin_frame, "()"),
    CANVAS_SYMBOL(canvas_end_frame, "()"),
    CANVAS_SYMBOL(canvas_get_width, "()"),
    CANVAS_SYMBOL(canvas_get_height, "()"),

    /* Style */
    CANVAS_SYMBOL(canvas_set_fill_style, "(iiii)"),
    CANVAS_SYMBOL(canvas_set_stroke_style, "(iiii)"),
    CANVAS_SYMBOL(canvas_set_global_alpha, "(f)"),
    CANVAS_SYMBOL(canvas_set_line_width, "(f)"),

    /* Rectangles */
    CANVAS_SYMBOL(canvas_fill_rect, "(ffff)"),
    CANVAS_SYMBOL(canvas_clear_rect, "(ffff)"),
    CANVAS_SYMBOL(canvas_stroke_rect, "(ffff)"),

    /* Paths */
    CANVAS_SYMBOL(canvas_begin_path, "()"),
    CANVAS_SYMBOL(canvas_move_to, "(ff)"),
    CANVAS_SYMBOL(canvas_line_to, "(ff)"),
    CANVAS_SYMBOL(canvas_stroke, "()"),
    CANVAS_SYMBOL(canvas_fill, "()"),
    CANVAS_SYMBOL(canvas_close_path, "()"),
    CANVAS_SYMBOL(canvas_arc, "(fffff)"),
    CANVAS_SYMBOL(canvas_rect, "(ffff)"),

    /* Text */
    CANVAS_SYMBOL(canvas_set_font, "(i)"),
    CANVAS_SYMBOL(canvas_fill_text, "(iff)"),
    CANVAS_SYMBOL(canvas_set_text_align, "(i)"),
    CANVAS_SYMBOL(canvas_measure_text, "(i)"),

    /* Image */
    CANVAS_SYMBOL(canvas_load_image, "(i)"),
    CANVAS_SYMBOL(canvas_image_get_width, "(i)"),
    CANVAS_SYMBOL(canvas_image_get_height, "(i)"),
    CANVAS_SYMBOL(canvas_draw_image, "(iff)"),
    CANVAS_SYMBOL(canvas_draw_image_scaled, "(iffff)"),
    CANVAS_SYMBOL(canvas_draw_image_frame, "(iffiiii)"),
    CANVAS_SYMBOL(canvas_draw_image_frame_scaled, "(iffffiiii)"),
    CANVAS_SYMBOL(canvas_unload_image, "(i)"),

    /* Pixel Operations */
    CANVAS_SYMBOL(canvas_put_pixel, "(iiiiii)"),
    CANVAS_SYMBOL(canvas_get_pixel, "(ii)"),

    /* Transform */
    CANVAS_SYMBOL(canvas_translate, "(ff)"),
    CANVAS_SYMBOL(canvas_rotate, "(f)"),
    CANVAS_SYMBOL(canvas_scale, "(ff)"),
    CANVAS_SYMBOL(canvas_save, "()"),
    CANVAS_SYMBOL(canvas_restore, "()"),
    CANVAS_SYMBOL(canvas_reset_transform, "()"),
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void canvas_api_init(uint16_t *fb, int width, int height)
{
    memset(&s_canvas, 0, sizeof(s_canvas));
    s_canvas.fb = fb;
    s_canvas.fb_width = width;
    s_canvas.fb_height = height;
    s_canvas.fill_r = 255;
    s_canvas.fill_g = 255;
    s_canvas.fill_b = 255;
    s_canvas.fill_a = 255;
    s_canvas.stroke_r = 0;
    s_canvas.stroke_g = 0;
    s_canvas.stroke_b = 0;
    s_canvas.stroke_a = 255;
    s_canvas.global_alpha = 1.0f;
    s_canvas.line_width = 1.0f;
    s_canvas.scale_x = 1.0f;
    s_canvas.scale_y = 1.0f;
    s_canvas.text_align = 0;
    s_canvas.next_image_id = 1;
    s_canvas.initialized = true;

    ESP_LOGI(TAG, "Canvas API initialized: %dx%d RGB565", width, height);
}

bool canvas_api_register(void)
{
    /* Register under both "xiaomiao" (game API namespace) and "env"
     * (default WASM import namespace for simple test programs). */
    bool ok = wasm_runtime_register_natives("xiaomiao",
                                            s_canvas_symbols,
                                            sizeof(s_canvas_symbols) / sizeof(NativeSymbol));
    ok = ok && wasm_runtime_register_natives("env",
                                              s_canvas_symbols,
                                              sizeof(s_canvas_symbols) / sizeof(NativeSymbol));
    return ok;
}

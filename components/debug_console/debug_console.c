/**
 * adb-like serial debug console for the Xiaomiao ESP32 + TinyGL.
 *
 * Spawns an ESP-IDF REPL on UART0 (the console UART). Commands probe the live
 * TinyGL rasterizer state (framebuffer pixels, texture pixmap, GL config) and
 * drive the ST7735 with test patterns — to debug the 3D pipeline without an
 * attached debugger. Built specifically to chase the "cube shows green→red
 * gradient instead of ceramic beige" texture color bug.
 *
 * Live state is reached via gl_get_context()->zb (public, static-inline in
 * tinygl's zgl.h) — no dependency on the main app component, no cycle.
 */

#include "debug_console.h"

#include "zgl.h"          /* gl_get_context, GLContext, gl_ctx (extern) */
#include "zbuffer.h"      /* ZBuffer: pbuf, current_texture, xsize/ysize */
#include "zfeatures.h"    /* TGL_PIXEL_BYTE_SWAP, TGL_TEXTURE_BYTE_SWAP */

#include "hw_display.h"   /* hw_display_flush, LCD_H/V_RES */

#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "dbgcon";

/* Defined in main/tinygl_test.c — when non-zero the render loop yields
 * without touching the framebuffer, so debug commands can paint and flush
 * without a race. */
extern volatile int tinygl_render_paused;
extern volatile int tinygl_cube_count;
extern volatile int tinygl_log_enabled;
extern volatile float tinygl_last_fps;
extern volatile int tinygl_physics_mode;
extern int tinygl_spawn_cube(void);

/* ── live state helpers ────────────────────────────────────────────────── */
static ZBuffer *get_zb(void)
{
    return gl_get_context()->zb;
}

/* Decode a 16-bit RGB565 word to 8-bit R,G,B. If TGL_PIXEL_BYTE_SWAP, the
 * stored word is byte-swapped relative to the logical RGB565, so un-swap
 * before decoding to report the intended color. */
static void decode565(uint16_t w, int *r, int *g, int *b)
{
#if TGL_PIXEL_BYTE_SWAP
    w = (uint16_t)((w << 8) | (w >> 8));
#endif
    *r = (w >> 11) & 0x1F;
    *g = (w >> 5)  & 0x3F;
    *b =  w        & 0x1F;
    /* scale to 0..255 */
    *r = (*r << 3) | (*r >> 2);
    *g = (*g << 2) | (*g >> 4);
    *b = (*b << 3) | (*b >> 2);
}

/* ── fps tracker (sampled by the `fps` command over a 1s window) ──────── */
static volatile int s_dbg_frames = 0;
void debug_console_tick_frame(void) { s_dbg_frames++; }  /* optional hook */

/* ── commands ──────────────────────────────────────────────────────────── */

static int cmd_state(int argc, char **argv)
{
    (void)argc; (void)argv;
    ZBuffer *zb = get_zb();
    if (!zb) { printf("zb == NULL (glInit not done yet)\n"); return 1; }
    printf("ZBuffer   : %p\n", (void *)zb);
    printf("  pbuf        = %p\n", (void *)zb->pbuf);
    printf("  zbuf        = %p\n", (void *)zb->zbuf);
    printf("  current_tex = %p\n", (void *)zb->current_texture);
    printf("  xsize/ysize = %d x %d\n", zb->xsize, zb->ysize);
    printf("config    : TGL_PIXEL_BYTE_SWAP=%d  TGL_TEXTURE_BYTE_SWAP=%d\n",
           TGL_PIXEL_BYTE_SWAP, TGL_TEXTURE_BYTE_SWAP);
    printf("render    : RENDER_BITS=%d  TEXTURE_DIM=%d\n",
           TGL_FEATURE_RENDER_BITS, TGL_FEATURE_TEXTURE_DIM);
    /* texture object via context */
    GLContext *c = gl_get_context();
    printf("ctx      : current_texture=%p  texture_2d_enabled=%d\n",
           (void *)c->current_texture, c->texture_2d_enabled);
    printf("render   : paused=%d\n", tinygl_render_paused);
    return 0;
}

static int cmd_fb(int argc, char **argv)
{
    int x = 80, y = 64;
    if (argc >= 3) { x = atoi(argv[1]); y = atoi(argv[2]); }
    ZBuffer *zb = get_zb();
    if (!zb) { printf("zb NULL\n"); return 1; }
    if (x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) {
        printf("out of range (0..%d, 0..%d)\n", LCD_H_RES - 1, LCD_V_RES - 1);
        return 1;
    }
    uint16_t w = (uint16_t)zb->pbuf[y * zb->xsize + x];
    int r, g, b;
    decode565(w, &r, &g, &b);
    printf("fb[%d,%d] = 0x%04x  -> R%d G%d B%d\n", x, y, w, r, g, b);
    return 0;
}

static int cmd_fbdump(int argc, char **argv)
{
    int x0 = 0, y0 = 0, x1 = 15, y1 = 7;
    if (argc >= 5) { x0 = atoi(argv[1]); y0 = atoi(argv[2]);
                     x1 = atoi(argv[3]); y1 = atoi(argv[4]); }
    ZBuffer *zb = get_zb();
    if (!zb) { printf("zb NULL\n"); return 1; }
    if (x1 < x0 || y1 < y0) { printf("bad rect\n"); return 1; }
    for (int y = y0; y <= y1; y++) {
        printf("%2d: ", y);
        for (int x = x0; x <= x1; x++) {
            uint16_t w = (uint16_t)zb->pbuf[y * zb->xsize + x];
            printf("%04x ", w);
        }
        printf("\n");
    }
    return 0;
}

static int cmd_tex(int argc, char **argv)
{
    int idx = 0;
    if (argc >= 2) idx = atoi(argv[1]);
    ZBuffer *zb = get_zb();
    if (!zb || !zb->current_texture) { printf("no texture bound\n"); return 1; }
    uint16_t w = (uint16_t)zb->current_texture[idx];
    int r, g, b;
    decode565(w, &r, &g, &b);
    printf("tex[%d] = 0x%04x -> R%d G%d B%d\n", idx, w, r, g, b);
    return 0;
}

static int cmd_texdump(int argc, char **argv)
{
    int n = 16;
    if (argc >= 2) n = atoi(argv[1]);
    if (n > TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM) n = TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM;
    ZBuffer *zb = get_zb();
    if (!zb || !zb->current_texture) { printf("no texture bound\n"); return 1; }
    for (int i = 0; i < n; i++) {
        uint16_t w = (uint16_t)zb->current_texture[i];
        int r, g, b; decode565(w, &r, &g, &b);
        printf("[%4d] 0x%04x R%d G%d B%d   %s",
               i, w, r, g, b, (i % 4 == 3) ? "\n" : "");
    }
    printf("\n");
    return 0;
}

/* Build a logical RGB565 word from 8-bit RGB, applying the build's byte-swap
 * so a write to pbuf flushes as the intended color (matches how TinyGL
 * stores pixels). */
static uint16_t rgb_to_pbuf(int r8, int g8, int b8)
{
    uint16_t c = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
#if TGL_PIXEL_BYTE_SWAP
    c = (uint16_t)((c << 8) | (c >> 8));
#endif
    return c;
}

static int cmd_clear(int argc, char **argv)
{
    if (argc < 4) { printf("usage: clear <r> <g> <b>  (0-255)\n"); return 1; }
    int r = atoi(argv[1]), g = atoi(argv[2]), b = atoi(argv[3]);
    ZBuffer *zb = get_zb();
    if (!zb) { printf("zb NULL\n"); return 1; }
    uint16_t px = rgb_to_pbuf(r, g, b);
    tinygl_render_paused = 1;   /* freeze render loop to avoid races */
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int i = 0; i < zb->xsize * zb->ysize; i++) zb->pbuf[i] = (PIXEL)px;
    hw_display_flush(0, 0, LCD_H_RES - 1, LCD_V_RES - 1, (const uint8_t *)zb->pbuf);
    printf("cleared R%d G%d B%d (pbuf 0x%04x), flushed\n", r, g, b, px);
    /* leave paused so the user can read the screen; use `resume` to restart */
    return 0;
}

static int cmd_rect(int argc, char **argv)
{
    if (argc < 8) {
        printf("usage: rect <x> <y> <w> <h> <r> <g> <b>\n");
        return 1;
    }
    int x = atoi(argv[1]), y = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
    int r = atoi(argv[5]), g = atoi(argv[6]), b = atoi(argv[7]);
    ZBuffer *zb = get_zb();
    if (!zb) { printf("zb NULL\n"); return 1; }
    uint16_t px = rgb_to_pbuf(r, g, b);
    tinygl_render_paused = 1;
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= LCD_V_RES) continue;
        for (int i = 0; i < w; i++) {
            int pxp = x + i;
            if (pxp < 0 || pxp >= LCD_H_RES) continue;
            zb->pbuf[py * zb->xsize + pxp] = (PIXEL)px;
        }
    }
    hw_display_flush(0, 0, LCD_H_RES - 1, LCD_V_RES - 1, (const uint8_t *)zb->pbuf);
    printf("rect %d,%d %dx%d R%dG%dB%d drawn+flushed\n", x, y, w, h, r, g, b);
    return 0;
}

static int cmd_pause(int argc, char **argv)
{
    (void)argc; (void)argv;
    tinygl_render_paused = 1;
    printf("render loop paused (framebuffer frozen)\n");
    return 0;
}

static int cmd_resume(int argc, char **argv)
{
    (void)argc; (void)argv;
    tinygl_render_paused = 0;
    printf("render loop resumed\n");
    return 0;
}

/* shot — dump the live framebuffer as compact hex, 16 pixels per line.
 * The host reads between ===SHOT BEGIN w h=== and ===SHOT END===,
 * each line is 32 hex chars = 16 little-endian RGB565 pixels.
 * Total lines = h, total chars ≈ h*33 = 4224 for 128 lines — fast. */
static int cmd_shot(int argc, char **argv)
{
    (void)argc; (void)argv;
    ZBuffer *zb = get_zb();
    if (!zb) { printf("zb NULL\n"); return 1; }
    tinygl_render_paused = 1;
    vTaskDelay(pdMS_TO_TICKS(20));   /* let the render loop go idle */
    printf("===SHOT BEGIN %d %d===\n", zb->xsize, zb->ysize);
    for (int y = 0; y < zb->ysize; y++) {
        for (int x = 0; x < zb->xsize; x++) {
            uint16_t w = (uint16_t)zb->pbuf[y * zb->xsize + x];
            printf("%04x", w);
        }
        printf("\n");
        fflush(stdout);
        if ((y & 7) == 7) vTaskDelay(1); /* yield every 8 lines */
    }
    printf("===SHOT END===\n");
    /* leave paused; `resume` to restart */
    return 0;
}

static int cmd_fps(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Report the latest FPS measured by the render loop (updated every 5s).
     * Also note how many cubes are active so the number is meaningful. */
    printf("fps = %.1f  (cubes=%d, tris/frame=%d)\n",
           tinygl_last_fps, tinygl_cube_count, tinygl_cube_count * 6);
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: log <on|off>  — toggle benchmark ESP_LOG every 5s\n");
        printf("current: %s\n", tinygl_log_enabled ? "on" : "off");
        return 1;
    }
    if (strcmp(argv[1], "on") == 0) {
        tinygl_log_enabled = 1;
        printf("log = on (benchmark prints FPS every 5s)\n");
    } else if (strcmp(argv[1], "off") == 0) {
        tinygl_log_enabled = 0;
        printf("log = off (silent — use `fps` to query)\n");
    } else {
        printf("unknown: %s (use on|off)\n", argv[1]);
        return 1;
    }
    return 0;
}

static int cmd_cubes(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cubes <n>  — set number of textured cubes (1-16)\n");
        printf("current: %d cube(s)\n", tinygl_cube_count);
        return 1;
    }
    int n = atoi(argv[1]);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    tinygl_cube_count = n;
    printf("cubes = %d (triangles/frame ≈ %d)\n", n, n * 12);
    return 0;
}

/* ── registration ──────────────────────────────────────────────────────── */
static int cmd_physics(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: physics <on|off|drop>\n");
        printf("  on   — enable physics mode (cubes fall with gravity)\n");
        printf("  off  — disable physics mode (static grid)\n");
        printf("  drop — spawn a falling cube\n");
        printf("current: %s\n", tinygl_physics_mode ? "on" : "off");
        return 1;
    }
    if (strcmp(argv[1], "on") == 0) {
        tinygl_physics_mode = 1;
        printf("physics = on (set `cubes N` to render N falling bodies)\n");
    } else if (strcmp(argv[1], "off") == 0) {
        tinygl_physics_mode = 0;
        printf("physics = off (static grid)\n");
    } else if (strcmp(argv[1], "drop") == 0) {
        int id = tinygl_spawn_cube();
        if (id < 0) printf("physics: body pool full\n");
        else {
            printf("physics: dropped cube id=%d (auto-enable physics mode)\n", id);
            tinygl_physics_mode = 1;
        }
    } else {
        printf("unknown: %s (use on|off|drop)\n", argv[1]);
        return 1;
    }
    return 0;
}

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = {
        .command = name,
        .help = help,
        .hint = NULL,
        .func = fn,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void debug_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "dbg> ";
    repl_cfg.max_cmdline_length = 256;

    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();

    reg("state", "Dump live ZBuffer pointers + GL config", cmd_state);
    reg("fb",    "fb <x> <y>  — framebuffer pixel as RGB565", cmd_fb);
    reg("fbdump","fbdump <x0> <y0> <x1> <y1>  — hex dump rect", cmd_fbdump);
    reg("tex",   "tex <idx>  — texture pixmap pixel as RGB565", cmd_tex);
    reg("texdump","texdump <n>  — dump first n texture pixels", cmd_texdump);
    reg("clear", "clear <r> <g> <b>  — clear screen + flush (pauses render)", cmd_clear);
    reg("rect",  "rect <x> <y> <w> <h> <r> <g> <b>  — draw rect + flush", cmd_rect);
    reg("pause", "pause the render loop (freeze framebuffer)", cmd_pause);
    reg("resume","resume the render loop", cmd_resume);
    reg("shot",  "capture framebuffer as base64 RGB565 (host decodes to PNG)", cmd_shot);
    reg("cubes", "cubes <n>  — set number of textured cubes (1-16)", cmd_cubes);
    reg("physics", "physics <on|off|drop>  — physics mode / spawn cube", cmd_physics);
    reg("fps",   "show current FPS (queries render loop)", cmd_fps);
    reg("log",   "log <on|off>  — toggle benchmark ESP_LOG output", cmd_log);

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "debug console ready (type 'help')");
}

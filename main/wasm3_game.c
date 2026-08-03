/**
 * wasm3 WebAssembly game engine for Xiaomiao ESP32.
 *
 * Runs wasm bytecode on core 0 via the wasm3 interpreter. The wasm module is
 * the sole author of every rendered scene: it calls GL API lookalikes
 * (glBegin / glVertex3f / glRotatef / ...), which the host wrappers encode into
 * a per-frame GL command stream. Core 1 drains the latest published frame and
 * replays it against the real TinyGL context (single-slot latest-wins — if
 * core 1 can't keep up, older frames are simply dropped).
 */
#include "wasm3.h"
#include "m3_env.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "glcmd_stream.h"

#include "wasm_game.wasm.h"

static const char *TAG = "wasm3";
static IM3Runtime s_runtime = NULL;

/* ── GL command-stream encoder host functions (core 0) ────────────────── */
/* Textures are referenced by integer ID only (glBindTexture); texture data is
 * uploaded once in gl_init and never crosses the API. */

static m3ApiRawFunction(host_glBegin) {
    m3ApiGetArg(int, mode);
    glcmd_u8(GLCMD_BEGIN); glcmd_u32((uint32_t)mode);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glEnd) {
    glcmd_u8(GLCMD_END);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glVertex3f) {
    m3ApiGetArg(float, x); m3ApiGetArg(float, y); m3ApiGetArg(float, z);
    glcmd_u8(GLCMD_VERTEX3F); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glColor3f) {
    m3ApiGetArg(float, r); m3ApiGetArg(float, g); m3ApiGetArg(float, b);
    glcmd_u8(GLCMD_COLOR3F); glcmd_f32(r); glcmd_f32(g); glcmd_f32(b);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glNormal3f) {
    m3ApiGetArg(float, x); m3ApiGetArg(float, y); m3ApiGetArg(float, z);
    glcmd_u8(GLCMD_NORMAL3F); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glTexCoord2f) {
    m3ApiGetArg(float, s); m3ApiGetArg(float, t);
    glcmd_u8(GLCMD_TEXCOORD2F); glcmd_f32(s); glcmd_f32(t);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glMatrixMode) {
    m3ApiGetArg(int, m);
    glcmd_u8(GLCMD_MATRIX_MODE); glcmd_u32((uint32_t)m);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glLoadIdentity) {
    glcmd_u8(GLCMD_LOAD_IDENTITY);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glPushMatrix) {
    glcmd_u8(GLCMD_PUSH_MATRIX);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glPopMatrix) {
    glcmd_u8(GLCMD_POP_MATRIX);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glRotatef) {
    m3ApiGetArg(float, a); m3ApiGetArg(float, x); m3ApiGetArg(float, y); m3ApiGetArg(float, z);
    glcmd_u8(GLCMD_ROTATEF); glcmd_f32(a); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glTranslatef) {
    m3ApiGetArg(float, x); m3ApiGetArg(float, y); m3ApiGetArg(float, z);
    glcmd_u8(GLCMD_TRANSLATEF); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glBindTexture) {
    m3ApiGetArg(int, target); m3ApiGetArg(int, tex);
    glcmd_u8(GLCMD_BIND_TEXTURE); glcmd_u32((uint32_t)target); glcmd_u32((uint32_t)tex);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glActiveTexture) {
    m3ApiGetArg(int, unit);
    glcmd_u8(GLCMD_ACTIVE_TEXTURE); glcmd_u32((uint32_t)unit);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glTexEnvi) {
    m3ApiGetArg(int, target); m3ApiGetArg(int, pname); m3ApiGetArg(int, param);
    glcmd_u8(GLCMD_TEX_ENVI); glcmd_u32((uint32_t)target); glcmd_u32((uint32_t)pname); glcmd_u32((uint32_t)param);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glTexEnvfv) {
    m3ApiGetArg(int, target); m3ApiGetArg(int, pname);
    m3ApiGetArg(float, v0); m3ApiGetArg(float, v1); m3ApiGetArg(float, v2); m3ApiGetArg(float, v3);
    glcmd_u8(GLCMD_TEX_ENVFV); glcmd_u32((uint32_t)target); glcmd_u32((uint32_t)pname);
    glcmd_f32(v0); glcmd_f32(v1); glcmd_f32(v2); glcmd_f32(v3);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glTexOffset) {
    m3ApiGetArg(int, unit); m3ApiGetArg(float, u); m3ApiGetArg(float, v);
    glcmd_u8(GLCMD_TEX_OFFSET); glcmd_u32((uint32_t)unit); glcmd_f32(u); glcmd_f32(v);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glEnable) {
    m3ApiGetArg(int, cap);
    glcmd_u8(GLCMD_ENABLE); glcmd_u32((uint32_t)cap);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glDisable) {
    m3ApiGetArg(int, cap);
    glcmd_u8(GLCMD_DISABLE); glcmd_u32((uint32_t)cap);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glDepthMask) {
    m3ApiGetArg(int, flag);
    glcmd_u8(GLCMD_DEPTH_MASK); glcmd_u32((uint32_t)flag);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glClear) {
    m3ApiGetArg(int, mask);
    glcmd_u8(GLCMD_CLEAR); glcmd_u32((uint32_t)mask);
    m3ApiSuccess();
}
static m3ApiRawFunction(host_glFlush) {
    /* Publish the accumulated frame (latest-wins) and start a fresh one. */
    glcmd_publish();
    glcmd_begin_frame();
    m3ApiSuccess();
}

/* ── Link host functions ───────────────────────────────────────────────── */

static bool link_host_functions(IM3Module module)
{
    #define LINK(name, func, sig) \
        m3_LinkRawFunction(module, "env", name, sig, func)

    LINK("glBegin",          host_glBegin,          "v(i)");
    LINK("glEnd",            host_glEnd,            "v()");
    LINK("glVertex3f",       host_glVertex3f,       "v(fff)");
    LINK("glColor3f",        host_glColor3f,        "v(fff)");
    LINK("glNormal3f",       host_glNormal3f,       "v(fff)");
    LINK("glTexCoord2f",     host_glTexCoord2f,     "v(ff)");
    LINK("glMatrixMode",     host_glMatrixMode,     "v(i)");
    LINK("glLoadIdentity",   host_glLoadIdentity,   "v()");
    LINK("glPushMatrix",     host_glPushMatrix,     "v()");
    LINK("glPopMatrix",      host_glPopMatrix,      "v()");
    LINK("glRotatef",        host_glRotatef,        "v(ffff)");
    LINK("glTranslatef",     host_glTranslatef,     "v(fff)");
    LINK("glBindTexture",    host_glBindTexture,    "v(ii)");
    LINK("glActiveTexture",  host_glActiveTexture,  "v(i)");
    LINK("glTexEnvi",        host_glTexEnvi,        "v(iii)");
    LINK("glTexEnvfv",       host_glTexEnvfv,       "v(iiffff)");
    LINK("glTexOffset",      host_glTexOffset,      "v(iff)");
    LINK("glEnable",         host_glEnable,         "v(i)");
    LINK("glDisable",        host_glDisable,        "v(i)");
    LINK("glDepthMask",      host_glDepthMask,      "v(i)");
    LINK("glClear",          host_glClear,          "v(i)");
    LINK("glFlush",          host_glFlush,          "v()");

    ESP_LOGI(TAG, "Host GL functions linked (%d)", 22);
    return true;
}

/* ── WASM3 game task (core 0) ──────────────────────────────────────────── */

void wasm3_game_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wasm3 game task starting on core %d", xPortGetCoreID());

    IM3Environment env = m3_NewEnvironment();
    if (!env) { ESP_LOGE(TAG, "m3_NewEnvironment failed"); return; }

    s_runtime = m3_NewRuntime(env, 65536, NULL);
    if (!s_runtime) { ESP_LOGE(TAG, "m3_NewRuntime failed"); return; }

    IM3Module module;
    M3Result result = m3_ParseModule(env, &module, wasm_game_wasm, wasm_game_wasm_len);
    if (result) { ESP_LOGE(TAG, "Parse: %s", result); return; }
    ESP_LOGI(TAG, "WASM parsed (%u bytes)", wasm_game_wasm_len);

    result = m3_LoadModule(s_runtime, module);
    if (result) { ESP_LOGE(TAG, "Load: %s", result); return; }

    if (!link_host_functions(module)) { ESP_LOGE(TAG, "Link failed"); return; }

    IM3Function fn;
    result = m3_FindFunction(&fn, s_runtime, "game_init");
    if (!result) { m3_CallV(fn); ESP_LOGI(TAG, "game_init() done"); }
    else         { ESP_LOGE(TAG, "game_init: %s", result); }

    result = m3_FindFunction(&fn, s_runtime, "game_update");
    if (result) { ESP_LOGE(TAG, "game_update: %s", result); return; }

    glcmd_begin_frame();
    ESP_LOGI(TAG, "Game loop on core 0");
    int64_t last_us = esp_timer_get_time();
    int n = 0;
    while (1) {
        M3Result cr = m3_CallV(fn);
        n++;
        if (cr) {
            ESP_LOGE(TAG, "iter %d: %s", n, cr);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if ((n % 30) == 0) ESP_LOGI(TAG, "%d iters OK", n);

        int64_t now = esp_timer_get_time();
        int64_t el = now - last_us;
        last_us = now;
        vTaskDelay(pdMS_TO_TICKS(el < 33333 ? (33333 - el) / 1000 : 1));
    }
}

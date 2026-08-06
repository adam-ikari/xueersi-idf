/**
 * WAMR WebAssembly game engine for Xiaomiao ESP32.
 *
 * Runs wasm_game.wasm on core 0 via the WAMR fast interpreter (bounded
 * native stack — unlike wasm3's tail-call-dependent threaded interpreter).
 * The wasm calls GL lookalikes; the host natives encode them into the
 * glcmd_stream, which core 1 replays against the real TinyGL context.
 * WAMR runtime memory (module, linear memory, exec_env stack) is allocated
 * entirely from PSRAM via custom allocator — it is not a performance hotspot.
 * SRAM is left for the TinyGL render pipeline.
 *
 * Strategy: Alloc_With_Allocator + heap_caps_malloc(MALLOC_CAP_SPIRAM).
 * This bypasses os_malloc's 12-byte overhead + double-alignment that
 * conflicts with ESP-IDF's PSRAM TLSF heap. WAMR calls our allocator
 * directly, so there is no extra alignment wrapper.
 */
#include "wasm_export.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
#include <string.h>
#include "freertos/task.h"
#include "glcmd_stream.h"
#include "wasm_game.wasm.h"

static const char *TAG = "wasm_game";

/* ── GL command-stream encoder host functions (core 0) ────────────────── */
#define GLW_EMPTY(env) (void)(env)

static void host_glBegin(wasm_exec_env_t env, int32_t mode)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_BEGIN); glcmd_u32((uint32_t)mode); }
static void host_glEnd(wasm_exec_env_t env)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_END); }
static void host_glVertex3f(wasm_exec_env_t env, float x, float y, float z)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_VERTEX3F); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z); }
static void host_glColor3f(wasm_exec_env_t env, float r, float g, float b)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_COLOR3F); glcmd_f32(r); glcmd_f32(g); glcmd_f32(b); }
static void host_glNormal3f(wasm_exec_env_t env, float x, float y, float z)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_NORMAL3F); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z); }
static void host_glTexCoord2f(wasm_exec_env_t env, float s, float t)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_TEXCOORD2F); glcmd_f32(s); glcmd_f32(t); }
static void host_glMatrixMode(wasm_exec_env_t env, int32_t m)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_MATRIX_MODE); glcmd_u32((uint32_t)m); }
static void host_glLoadIdentity(wasm_exec_env_t env)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_LOAD_IDENTITY); }
static void host_glPushMatrix(wasm_exec_env_t env)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_PUSH_MATRIX); }
static void host_glPopMatrix(wasm_exec_env_t env)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_POP_MATRIX); }
static void host_glRotatef(wasm_exec_env_t env, float a, float x, float y, float z)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_ROTATEF); glcmd_f32(a); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z); }
static void host_glTranslatef(wasm_exec_env_t env, float x, float y, float z)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_TRANSLATEF); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z); }
static void host_glBindTexture(wasm_exec_env_t env, int32_t target, int32_t tex)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_BIND_TEXTURE); glcmd_u32((uint32_t)target); glcmd_u32((uint32_t)tex); }
static void host_glActiveTexture(wasm_exec_env_t env, int32_t unit)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_ACTIVE_TEXTURE); glcmd_u32((uint32_t)unit); }
static void host_glTexEnvi(wasm_exec_env_t env, int32_t target, int32_t pname, int32_t param)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_TEX_ENVI); glcmd_u32((uint32_t)target); glcmd_u32((uint32_t)pname); glcmd_u32((uint32_t)param); }
static void host_glTexEnvfv(wasm_exec_env_t env, int32_t target, int32_t pname,
                            float v0, float v1, float v2, float v3)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_TEX_ENVFV); glcmd_u32((uint32_t)target); glcmd_u32((uint32_t)pname);
  glcmd_f32(v0); glcmd_f32(v1); glcmd_f32(v2); glcmd_f32(v3); }
static void host_glTexOffset(wasm_exec_env_t env, int32_t unit, float u, float v)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_TEX_OFFSET); glcmd_u32((uint32_t)unit); glcmd_f32(u); glcmd_f32(v); }
static void host_glTexGeni(wasm_exec_env_t env, int32_t coord, int32_t pname, int32_t param)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_TEX_GENI); glcmd_u32((uint32_t)coord); glcmd_u32((uint32_t)pname); glcmd_u32((uint32_t)param); }
static void host_glSetEnableSpecular(wasm_exec_env_t env, int32_t flag)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_SET_ENABLE_SPECULAR); glcmd_u32((uint32_t)flag); }
static void host_glEnable(wasm_exec_env_t env, int32_t cap)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_ENABLE); glcmd_u32((uint32_t)cap); }
static void host_glDisable(wasm_exec_env_t env, int32_t cap)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_DISABLE); glcmd_u32((uint32_t)cap); }
static void host_glDepthMask(wasm_exec_env_t env, int32_t flag)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_DEPTH_MASK); glcmd_u32((uint32_t)flag); }
static void host_glClear(wasm_exec_env_t env, int32_t mask)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_CLEAR); glcmd_u32((uint32_t)mask); }

/* ── Lighting / Materials (new) ── */
static void host_glMaterialfv(wasm_exec_env_t env, int32_t mode, int32_t type,
                              float v0, float v1, float v2, float v3)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_MATERIAL_FV); glcmd_u32((uint32_t)mode);
  glcmd_u32((uint32_t)type); glcmd_f32(v0); glcmd_f32(v1); glcmd_f32(v2); glcmd_f32(v3); }
static void host_glMaterialf(wasm_exec_env_t env, int32_t mode, int32_t type, float v)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_MATERIAL_F); glcmd_u32((uint32_t)mode);
  glcmd_u32((uint32_t)type); glcmd_f32(v); }
static void host_glLightfv(wasm_exec_env_t env, int32_t light, int32_t type,
                           float v0, float v1, float v2, float v3)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_LIGHT_FV); glcmd_u32((uint32_t)light);
  glcmd_u32((uint32_t)type); glcmd_f32(v0); glcmd_f32(v1); glcmd_f32(v2); glcmd_f32(v3); }
static void host_glLightModeli(wasm_exec_env_t env, int32_t pname, int32_t param)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_LIGHT_MODEL_I); glcmd_u32((uint32_t)pname); glcmd_u32((uint32_t)param); }
static void host_glLightModelfv(wasm_exec_env_t env, int32_t pname,
                                float v0, float v1, float v2, float v3)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_LIGHT_MODEL_FV); glcmd_u32((uint32_t)pname);
  glcmd_f32(v0); glcmd_f32(v1); glcmd_f32(v2); glcmd_f32(v3); }
static void host_glColorMaterial(wasm_exec_env_t env, int32_t mode, int32_t type)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_COLOR_MATERIAL); glcmd_u32((uint32_t)mode); glcmd_u32((uint32_t)type); }

/* ── Transform / View (new) ── */
static void host_glScalef(wasm_exec_env_t env, float x, float y, float z)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_SCALE_F); glcmd_f32(x); glcmd_f32(y); glcmd_f32(z); }
static void host_glViewport(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w, int32_t h)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_VIEWPORT); glcmd_u32((uint32_t)x); glcmd_u32((uint32_t)y);
  glcmd_u32((uint32_t)w); glcmd_u32((uint32_t)h); }
static void host_glFrustum(wasm_exec_env_t env, double l, double r, double b, double t, double n, double f)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_FRUSTUM);
  uint64_t raw; memcpy(&raw, &l, 8); glcmd_u32((uint32_t)(raw)); glcmd_u32((uint32_t)(raw >> 32));
  memcpy(&raw, &r, 8); glcmd_u32((uint32_t)(raw)); glcmd_u32((uint32_t)(raw >> 32));
  memcpy(&raw, &b, 8); glcmd_u32((uint32_t)(raw)); glcmd_u32((uint32_t)(raw >> 32));
  memcpy(&raw, &t, 8); glcmd_u32((uint32_t)(raw)); glcmd_u32((uint32_t)(raw >> 32));
  memcpy(&raw, &n, 8); glcmd_u32((uint32_t)(raw)); glcmd_u32((uint32_t)(raw >> 32));
  memcpy(&raw, &f, 8); glcmd_u32((uint32_t)(raw)); glcmd_u32((uint32_t)(raw >> 32)); }
static void host_glShadeModel(wasm_exec_env_t env, int32_t mode)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_SHADE_MODEL); glcmd_u32((uint32_t)mode); }

/* ── Blend (new) ── */
static void host_glBlendFunc(wasm_exec_env_t env, int32_t sfactor, int32_t dfactor)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_BLEND_FUNC); glcmd_u32((uint32_t)sfactor); glcmd_u32((uint32_t)dfactor); }

/* ── Clear (new) ── */
static void host_glClearColor(wasm_exec_env_t env, float r, float g, float b, float a)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_CLEAR_COLOR); glcmd_f32(r); glcmd_f32(g); glcmd_f32(b); glcmd_f32(a); }

static void host_glFlush(wasm_exec_env_t env)
{ GLW_EMPTY(env); glcmd_publish(); glcmd_begin_frame(); }

/* math functions used by wasm_game.cpp (cosf/sinf for rotation trig) */
static float host_cosf(wasm_exec_env_t env, float x)
{ GLW_EMPTY(env); return cosf(x); }
static float host_sinf(wasm_exec_env_t env, float x)
{ GLW_EMPTY(env); return sinf(x); }

/* ── Key event ring buffer (written by core 1 render task, read by core 0 WASM host) ─ */
#define KEY_RING_SIZE 16
static volatile int32_t s_key_events[KEY_RING_SIZE];
static volatile int     s_key_rd;  /* WASM reads from here */
static volatile int     s_key_wr;  /* render task writes to here */

/** Push a key event into the ring.  Called from the render task (core 1).
 *  @param btn_idx  0=UP 1=DOWN 2=LEFT 3=RIGHT 4=A 5=B
 *  @param down     true = down edge, false = up edge */
void wasm_key_push(int btn_idx, int down)
{
    int next = (s_key_wr + 1) % KEY_RING_SIZE;
    if (next == s_key_rd) return;  /* full, drop oldest */
    /* Encode: (btn_idx << 2) | (down ? 0 : 1)  — matches wasm_game.cpp KEY_EDGE_* */
    s_key_events[s_key_wr] = ((int32_t)btn_idx << 2) | (down ? 0 : 1);
    s_key_wr = next;
}

static int32_t host_game_get_key(wasm_exec_env_t env)
{
    GLW_EMPTY(env);
    if (s_key_rd == s_key_wr) return 0;  /* empty */
    int32_t ev = s_key_events[s_key_rd];
    s_key_rd = (s_key_rd + 1) % KEY_RING_SIZE;
    return ev;
}

/* ── Native symbol table — signatures match wasm imports exactly ─────────
 * WAMR signature convention (wasm_native.c compare_type_with_signature):
 * i = i32, I = i64, f = f32, F = f64. The wasm module declares its GL
 * imports with C `float` (f32) → lowercase 'f'. */
static NativeSymbol gl_natives[] = {
    { "glBegin",         (void *)host_glBegin,         "(i)",      NULL },
    { "glEnd",           (void *)host_glEnd,           "()",       NULL },
    { "glVertex3f",      (void *)host_glVertex3f,      "(fff)",    NULL },
    { "glColor3f",       (void *)host_glColor3f,       "(fff)",    NULL },
    { "glNormal3f",      (void *)host_glNormal3f,      "(fff)",    NULL },
    { "glTexCoord2f",    (void *)host_glTexCoord2f,    "(ff)",     NULL },
    { "glMatrixMode",    (void *)host_glMatrixMode,    "(i)",      NULL },
    { "glLoadIdentity",  (void *)host_glLoadIdentity,  "()",       NULL },
    { "glPushMatrix",    (void *)host_glPushMatrix,    "()",       NULL },
    { "glPopMatrix",     (void *)host_glPopMatrix,     "()",       NULL },
    { "glRotatef",       (void *)host_glRotatef,       "(ffff)",   NULL },
    { "glTranslatef",    (void *)host_glTranslatef,    "(fff)",    NULL },
    { "glBindTexture",   (void *)host_glBindTexture,   "(ii)",     NULL },
    { "glActiveTexture", (void *)host_glActiveTexture, "(i)",      NULL },
    { "glTexEnvi",       (void *)host_glTexEnvi,       "(iii)",    NULL },
    { "glTexEnvfv",      (void *)host_glTexEnvfv,      "(iiffff)", NULL },
    { "glTexOffset",     (void *)host_glTexOffset,     "(iff)",    NULL },
    { "glTexGeni",       (void *)host_glTexGeni,       "(iii)",    NULL },
    { "glSetEnableSpecular", (void *)host_glSetEnableSpecular, "(i)", NULL },
    { "glEnable",        (void *)host_glEnable,        "(i)",      NULL },
    { "glDisable",       (void *)host_glDisable,       "(i)",      NULL },
    { "glDepthMask",     (void *)host_glDepthMask,     "(i)",      NULL },
    { "glClear",         (void *)host_glClear,         "(i)",      NULL },

    /* ── Lighting / Materials ── */
    { "glMaterialfv",    (void *)host_glMaterialfv,    "(iiffff)", NULL },
    { "glMaterialf",     (void *)host_glMaterialf,     "(iif)",    NULL },
    { "glLightfv",       (void *)host_glLightfv,       "(iiffff)", NULL },
    { "glLightModeli",   (void *)host_glLightModeli,   "(ii)",     NULL },
    { "glLightModelfv",  (void *)host_glLightModelfv,  "(iffff)",  NULL },
    { "glColorMaterial", (void *)host_glColorMaterial, "(ii)",     NULL },

    /* ── Transform / View ── */
    { "glScalef",        (void *)host_glScalef,        "(fff)",    NULL },
    { "glViewport",      (void *)host_glViewport,      "(iiii)",   NULL },
    { "glFrustum",       (void *)host_glFrustum,       "(FFFFFF)", NULL },
    { "glShadeModel",    (void *)host_glShadeModel,    "(i)",      NULL },

    /* ── Blend ── */
    { "glBlendFunc",     (void *)host_glBlendFunc,     "(ii)",     NULL },

    /* ── Clear ── */
    { "glClearColor",    (void *)host_glClearColor,    "(ffff)",   NULL },

    { "glFlush",         (void *)host_glFlush,         "()",       NULL },
    { "cosf",            (void *)host_cosf,            "(f)f",     NULL },
    { "sinf",            (void *)host_sinf,            "(f)f",     NULL },
    { "game_get_key",    (void *)host_game_get_key,    "()i",      NULL },
};
#define GL_NATIVES_COUNT (sizeof(gl_natives) / sizeof(gl_natives[0]))

/* ── PSRAM custom allocator for WAMR (avoids os_malloc double-alignment) ───
 * When Alloc_With_Allocator is used, WAMR calls these directly instead of
 * os_malloc → malloc, so there is no 12-byte overhead / double-alignment
 * conflict with ESP-IDF's PSRAM heap. This keeps SRAM free for TinyGL.
 *
 * WAMR's internal GC heap init (ems_kfc.c) requires 8-byte alignment on
 * the struct_buf and pool_buf it receives. heap_caps_malloc only guarantees
 * 4-byte on ESP32, so we over-allocate and manually align up.
 *
 * MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT: SPRAM preferred, SRAM as fallback. */
static void *wamr_psram_malloc(unsigned int size)
{
    if (size == 0) size = 1;
    void *p = heap_caps_malloc(size + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    uintptr_t a = ((uintptr_t)p + 8) & ~(uintptr_t)7;
    ((void **)a)[-1] = p;
    return (void *)a;
}
static void *wamr_psram_realloc(void *ptr, unsigned int size)
{
    if (!ptr) return wamr_psram_malloc(size);
    void *old_base = ((void **)ptr)[-1];
    void *p = heap_caps_realloc(old_base, size + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    uintptr_t a = ((uintptr_t)p + 8) & ~(uintptr_t)7;
    ((void **)a)[-1] = p;
    return (void *)a;
}
static void wamr_psram_free(void *ptr)
{
    if (ptr) heap_caps_free(((void **)ptr)[-1]);
}

/* ── WAMR game task (core 0) ──────────────────────────────────────────── */
void wasm_game_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "wasm_game task starting on core %d", xPortGetCoreID());

    RuntimeInitArgs init_args = { 0 };
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func  = wamr_psram_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = wamr_psram_realloc;
    init_args.mem_alloc_option.allocator.free_func    = wamr_psram_free;
    init_args.native_module_name = "env";
    init_args.native_symbols     = gl_natives;
    init_args.n_native_symbols   = (uint32_t)GL_NATIVES_COUNT;
    init_args.running_mode       = Mode_Interp;
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "wasm_runtime_full_init failed");
        return;
    }
    ESP_LOGI(TAG, "Host GL functions linked (%d)", GL_NATIVES_COUNT);

    char err[128] = { 0 };
    wasm_module_t mod = wasm_runtime_load((uint8_t *)wasm_game_wasm,
                                          wasm_game_wasm_len, err, sizeof(err));
    if (!mod) { ESP_LOGE(TAG, "Parse: %s", err); return; }
    ESP_LOGI(TAG, "WASM parsed (%u bytes)", wasm_game_wasm_len);

    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, 0, 4096, err, sizeof(err));
    if (!inst) { ESP_LOGE(TAG, "Instantiate: %s", err); return; }

    /* Exec_env operation stack — the interpreter's operand stack. PSRAM is
     * fine (game logic is one pass per frame). */
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16384);
    if (!env) { ESP_LOGE(TAG, "create_exec_env failed"); return; }

    wasm_function_inst_t fn = wasm_runtime_lookup_function(inst, "game_init");
    if (!fn) { ESP_LOGE(TAG, "game_init not found"); return; }
    wasm_runtime_call_wasm(env, fn, 0, NULL);
    ESP_LOGI(TAG, "game_init() done");

    fn = wasm_runtime_lookup_function(inst, "game_update");
    if (!fn) { ESP_LOGE(TAG, "game_update not found"); return; }

    glcmd_begin_frame();
    ESP_LOGI(TAG, "Game loop on core 0");
    int64_t last_us = esp_timer_get_time();
    int n = 0;
    while (1) {
        if (!wasm_runtime_call_wasm(env, fn, 0, NULL)) {
            const char *e = wasm_runtime_get_exception(inst);
            ESP_LOGE(TAG, "iter %d: %s", n, e ? e : "unknown");
            wasm_runtime_clear_exception(inst);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        n++;
        if ((n % 30) == 0) ESP_LOGI(TAG, "%d iters OK", n);

        int64_t now = esp_timer_get_time();
        int64_t el = now - last_us;
        last_us = now;
        vTaskDelay(pdMS_TO_TICKS(el < 33333 ? (33333 - el) / 1000 : 1));
    }
}

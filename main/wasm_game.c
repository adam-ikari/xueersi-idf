/**
 * WAMR WebAssembly game engine for Xiaomiao ESP32.
 *
 * Runs wasm_game.wasm on core 0 via the WAMR fast interpreter (bounded
 * native stack — unlike wasm3's tail-call-dependent threaded interpreter).
 * The wasm calls GL lookalikes; the host natives encode them into the
 * glcmd_stream, which core 1 replays against the real TinyGL context.
 * WAMR runtime memory (module, linear memory, exec_env stack) is allocated
 * entirely from PSRAM — it is not a performance hotspot.
 */
#include "wasm_export.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <math.h>
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
static void host_glEnable(wasm_exec_env_t env, int32_t cap)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_ENABLE); glcmd_u32((uint32_t)cap); }
static void host_glDisable(wasm_exec_env_t env, int32_t cap)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_DISABLE); glcmd_u32((uint32_t)cap); }
static void host_glDepthMask(wasm_exec_env_t env, int32_t flag)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_DEPTH_MASK); glcmd_u32((uint32_t)flag); }
static void host_glClear(wasm_exec_env_t env, int32_t mask)
{ GLW_EMPTY(env); glcmd_u8(GLCMD_CLEAR); glcmd_u32((uint32_t)mask); }
static void host_glFlush(wasm_exec_env_t env)
{ GLW_EMPTY(env); glcmd_publish(); glcmd_begin_frame(); }

/* math functions used by wasm_game.cpp (cosf/sinf for rotation trig) */
static float host_cosf(wasm_exec_env_t env, float x)
{ GLW_EMPTY(env); return cosf(x); }
static float host_sinf(wasm_exec_env_t env, float x)
{ GLW_EMPTY(env); return sinf(x); }

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
    { "glEnable",        (void *)host_glEnable,        "(i)",      NULL },
    { "glDisable",       (void *)host_glDisable,       "(i)",      NULL },
    { "glDepthMask",     (void *)host_glDepthMask,     "(i)",      NULL },
    { "glClear",         (void *)host_glClear,         "(i)",      NULL },
    { "glFlush",         (void *)host_glFlush,         "()",       NULL },
    { "cosf",            (void *)host_cosf,            "(f)f",     NULL },
    { "sinf",            (void *)host_sinf,            "(f)f",     NULL },
};
#define GL_NATIVES_COUNT (sizeof(gl_natives) / sizeof(gl_natives[0]))

/* ── WAMR allocator: runtime memory → PSRAM (not a hot spot) ───────────── */
static void *wamr_malloc(size_t size)
{
    /* WAMR requires 8-byte aligned heap structures. PSRAM heap_caps_malloc
     * guarantees only 4 bytes on ESP32. Align manually. */
    void *p = heap_caps_malloc(size + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    uintptr_t a = ((uintptr_t)p + 8) & ~(uintptr_t)7;
    ((void **)a)[-1] = p;
    return (void *)a;
}
static void *wamr_realloc(void *ptr, size_t size)
{
    if (!ptr) return wamr_malloc(size);
    void *old_base = ((void **)ptr)[-1];
    void *p = heap_caps_realloc(old_base, size + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    uintptr_t a = ((uintptr_t)p + 8) & ~(uintptr_t)7;
    ((void **)a)[-1] = p;
    return (void *)a;
}
static void wamr_free(void *ptr)
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
    init_args.mem_alloc_option.allocator.malloc_func  = wamr_malloc;
    init_args.mem_alloc_option.allocator.realloc_func = wamr_realloc;
    init_args.mem_alloc_option.allocator.free_func    = wamr_free;
    init_args.native_module_name = "env";
    init_args.native_symbols     = gl_natives;
    init_args.n_native_symbols   = (uint32_t)GL_NATIVES_COUNT;
    init_args.running_mode       = Mode_Interp;
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE(TAG, "wasm_runtime_full_init failed");
        return;
    }
    ESP_LOGI(TAG, "Host GL functions linked (%d)", GL_NATIVES_COUNT);
    ESP_LOGI(TAG, "heap: internal_free=%u psram_free=%u (WAMR runtime→PSRAM allocator)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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

# WAMR 引擎替换 + 图形优先内存实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 WAMR 替换 wasm3 引擎（同一 GL 命令流桥），显示路径升级为 N 帧缓冲流水线，内存按"图形引擎 SRAM 优先"分配。

**Architecture:** wasm_game.c（WAMR fast interp，core0）解释同一份 wasm_game.wasm，22 个 GL 原生编码进 glcmd_stream；core1 重放 TinyGL 渲染进 N 帧缓冲流水线并 DMA 显示。WAMR 运行时全部走 PSRAM（`wasm_runtime_full_init` + 自定义分配器），图形引擎缓冲锁定 SRAM。

**Tech Stack:** ESP-IDF v5.5（esp32）、WAMR（espressif/wasm-micro-runtime，fast interp）、TinyGL、FreeRTOS 双核。

## Global Constraints

- ESP-IDF v5.5.4，target esp32，xtensa GCC 14.2.0。
- 构建验证：`source /home/gem/esp/esp-idf/export.sh && idf.py build`。
- 硬件验证：烧录后观察串口（`idf.py -p /dev/ttyACM0 -b 460800 flash monitor`）。本仓库无单元测试，验证靠构建 + 硬件。
- `wasm_game.wasm`（构建期由 `wasm_game.cpp` 经 clang++ → wasm-ld 生成）**一字不改**，其导入/导出签名见 §Task1。
- WAMR 组件 `espressif__wasm-micro-runtime` 已在 `main/CMakeLists.txt` 的 `PRIV_REQUIRES` 中（勿删）。
- 所有 GL 原生函数签名必须与 wasm 导入**严格一致**（WAMR 会校验）。

---

### Task 1: WAMR 游戏引擎替换

**Files:**
- Create: `main/wasm_game.c`, `main/wasm_game.h`
- Modify: `main/CMakeLists.txt`（SRCS `wasm3_game.c`→`wasm_game.c`；PRIV_REQUIRES 删 `wasm3`）
- Modify: `main/tinygl_test.c`（任务名、栈还原）
- Modify: `sdkconfig.defaults`（删 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`）
- Delete: `main/wasm3_game.c`, `main/wasm3_game.h`, `main/wasm_game.c`（旧 WAMR 示例死代码）, `components/wasm3/`（整个目录）

**Interfaces:**
- Consumes: `glcmd_stream.h`（现有旧 API：`glcmd_begin_frame/glcmd_u8/glcmd_u32/glcmd_f32/glcmd_publish`）、`wasm_game.wasm.h`（构建期生成，符号 `wasm_game_wasm` / `wasm_game_wasm_len`）、WAMR `wasm_export.h`
- Produces: `void wasm_game_task(void *arg)`（core0 任务，`tinygl_benchmark` 调用）

- [ ] **Step 1: 新建 `main/wasm_game.h`**

```c
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/** WAMR game task — runs on core 0. */
void wasm_game_task(void *arg);
#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: 新建 `main/wasm_game.c`（WAMR 引擎）**

```c
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

/* ── Native symbol table — signatures match wasm imports exactly ───────── */
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
};
#define GL_NATIVES_COUNT (sizeof(gl_natives) / sizeof(gl_natives[0]))

/* ── WAMR allocator: runtime memory → PSRAM (not a hot spot) ───────────── */
static void *wamr_malloc(size_t size)
{ return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *wamr_realloc(void *ptr, size_t size)
{ return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void wamr_free(void *ptr)
{ heap_caps_free(ptr); }

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
```

- [ ] **Step 3: 改 `main/tinygl_test.c` — 任务改名 + 还原 SRAM 栈**

把 `tinygl_benchmark` 里 `#ifdef TGL_WASM_GAME` 块（约 640–670 行）改为：

```c
#ifdef TGL_WASM_GAME
    /* Core 0: wasm game task — the sole author of the scene via GL commands.
     * WAMR fast interpreter has a bounded native stack (no per-opcode growth
     * like wasm3), so a plain 32KB SRAM stack suffices. */
    xTaskCreatePinnedToCore(
        wasm_game_task,
        "wasm_game",
        8192,   /* 32KB = 8192 words on xtensa (StackType_t=4B) */
        NULL,
        configMAX_PRIORITIES - 2,
        NULL,
        0                           /* core 0 */
    );
#else
```

同时删除文件顶部 `#if defined(TGL_WASM_GAME) && !defined(TGL_EMU_BUILD)` 块（`WASM3_GAME_STACK_BYTES` / `s_wasm3_tcb` / `s_wasm3_stack`，约 61–72 行）以及 `esp_heap_caps.h` include（若不再使用）。`#include "wasm3_game.h"`（约 41 行）改为 `#include "wasm_game.h"`。

- [ ] **Step 4: 改 `main/CMakeLists.txt`**

- SRCS 里 `"wasm3_game.c"` → `"wasm_game.c"`
- `PRIV_REQUIRES` 里删除 `wasm3`（保留 `wasm-micro-runtime`）

- [ ] **Step 5: 改 `sdkconfig.defaults`**

删除 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` 及其上的注释块。

- [ ] **Step 6: 删除旧文件**

注意：旧的 `main/wasm_game.c`（未编译的 WAMR 示例）会被 Step 2 新建的同名文件**覆盖**——它是死代码，无需单独删。只删 wasm3 相关：

```bash
git rm main/wasm3_game.c main/wasm3_game.h
rm -rf components/wasm3
```

- [ ] **Step 7: 构建验证**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py build`
Expected: 编译通过；`wasm_game.c` 编译、`wasm3` 不再出现在 component 列表；链接成功生成 `build/xiaomiao.bin`。若报 `wasm3` 未找到依赖，确认 CMakeLists 已删干净。

- [ ] **Step 8: 硬件验证**

烧录后串口应看到：
```
wasm_game: wasm_game task starting on core 0
wasm_game: Host GL functions linked (22)
wasm_game: WASM parsed (5026 bytes)
wasm_game: game_init() done
wasm_game: Game loop on core 0
```
无 `A stack overflow` 错误，30fps 场景正常渲染。

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "feat(wasm): run game on WAMR, drop wasm3 + PSRAM stack hack"
```

---

### Task 2: glcmd N 缓冲零拷贝同步

**Files:**
- Modify: `main/glcmd_stream.h`, `main/glcmd_stream.c`
- Modify: `main/wasm_game.c`（编码端，`glcmd_begin_frame/publish` 语义不变）
- Modify: `main/tinygl_test.c`（解码端 `render_frame` 用新 poll/release API）

**Interfaces:**
- Consumes: Task 1 的 `wasm_game.c`
- Produces:
  - 编码端：`glcmd_begin_frame(void)` / `glcmd_publish(void)` / `bool glcmd_u8(u8)` / `bool glcmd_u32(u32)` / `bool glcmd_f32(float)`（签名不变）
  - 解码端：`const uint8_t *glcmd_frame_poll(uint32_t *out_len)` / `void glcmd_frame_release(void)` / `uint32_t glcmd_replay(const uint8_t*, uint32_t)`
  - `#define GLCMD_NUM_BUFFERS 2`（可配 2 或 3）

- [ ] **Step 1: 重写 `main/glcmd_stream.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GLCMD_BUFFER_SIZE 8192
/* Number of ping-pong frame buffers (2 = paced, 3 = extra jitter slack). */
#define GLCMD_NUM_BUFFERS 2

/* ── Encoder (core 0, wasm game) ── */
void glcmd_begin_frame(void);          /* wait for a free buffer, reset pos */
void glcmd_publish(void);              /* publish current buffer, advance */
bool glcmd_u8(uint8_t v);
bool glcmd_u32(uint32_t v);
bool glcmd_f32(float v);

/* ── Decoder (core 1, render) ── */
const uint8_t *glcmd_frame_poll(uint32_t *out_len);  /* NULL if this cycle's
                                                        buffer has no frame */
void glcmd_frame_release(void);        /* mark consumed, advance dec_idx */
uint32_t glcmd_replay(const uint8_t *buf, uint32_t len);
```

- [ ] **Step 2: 重写 `main/glcmd_stream.c`**

```c
#include "glcmd_stream.h"
#include "GL/gl.h"
#include "zgl.h"
#include <string.h>

/* N buffers; producer (core 0) and consumer (core 1) each own their own
 * index, so no lock is needed. s_len[i]==0 → buffer free for the producer. */
static uint8_t s_buf[GLCMD_NUM_BUFFERS][GLCMD_BUFFER_SIZE];
static volatile uint32_t s_len[GLCMD_NUM_BUFFERS];
static uint32_t s_pos = 0;
static uint32_t s_enc_idx = 0;   /* written only by core 0 */
static uint32_t s_dec_idx = 0;   /* written only by core 1 */

void glcmd_begin_frame(void)
{
    /* Steady state (30fps lockstep): already drained → no spin. */
    while (s_len[s_enc_idx] != 0) { }
    s_pos = 0;
}

void glcmd_publish(void)
{
    s_len[s_enc_idx] = s_pos;
    __sync_synchronize();              /* make frame data visible to core 1 */
    s_enc_idx = (s_enc_idx + 1) % GLCMD_NUM_BUFFERS;
}

bool glcmd_u8(uint8_t v)
{
    if (s_pos < GLCMD_BUFFER_SIZE) { s_buf[s_enc_idx][s_pos++] = v; return true; }
    return false;
}

bool glcmd_u32(uint32_t v)
{
    if (s_pos + 4 <= GLCMD_BUFFER_SIZE) {
        memcpy(s_buf[s_enc_idx] + s_pos, &v, 4); s_pos += 4; return true;
    }
    return false;
}

bool glcmd_f32(float v)
{
    uint32_t r; memcpy(&r, &v, 4); return glcmd_u32(r);
}

const uint8_t *glcmd_frame_poll(uint32_t *out_len)
{
    uint32_t len = s_len[s_dec_idx];
    if (!len) return NULL;
    *out_len = len;
    return s_buf[s_dec_idx];
}

void glcmd_frame_release(void)
{
    s_len[s_dec_idx] = 0;
    __sync_synchronize();
    s_dec_idx = (s_dec_idx + 1) % GLCMD_NUM_BUFFERS;
}

/* ── Decoder: replay one frame (unchanged body) ── */
static inline uint32_t read_u32(const uint8_t **p) { uint32_t v; memcpy(&v,*p,4); *p+=4; return v; }
static inline float read_f32(const uint8_t **p) { float v; uint32_t raw; memcpy(&raw,*p,4); memcpy(&v,&raw,4); *p+=4; return v; }

uint32_t glcmd_replay(const uint8_t *buf, uint32_t len)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;
    while (p < end) {
        uint8_t op = *p++;
        switch (op) {
        case GLCMD_BEGIN: glBegin((GLint)read_u32(&p)); break;
        case GLCMD_END: glEnd(); break;
        case GLCMD_VERTEX3F: glVertex3f(read_f32(&p), read_f32(&p), read_f32(&p)); break;
        case GLCMD_COLOR3F: glColor3f(read_f32(&p), read_f32(&p), read_f32(&p)); break;
        case GLCMD_NORMAL3F: glNormal3f(read_f32(&p), read_f32(&p), read_f32(&p)); break;
        case GLCMD_TEXCOORD2F: glTexCoord2f(read_f32(&p), read_f32(&p)); break;
        case GLCMD_MATRIX_MODE: glMatrixMode((GLint)read_u32(&p)); break;
        case GLCMD_LOAD_IDENTITY: glLoadIdentity(); break;
        case GLCMD_PUSH_MATRIX: glPushMatrix(); break;
        case GLCMD_POP_MATRIX: glPopMatrix(); break;
        case GLCMD_ROTATEF: glRotatef(read_f32(&p), read_f32(&p), read_f32(&p), read_f32(&p)); break;
        case GLCMD_TRANSLATEF: glTranslatef(read_f32(&p), read_f32(&p), read_f32(&p)); break;
        case GLCMD_BIND_TEXTURE: { GLint t=(GLint)read_u32(&p); GLint x=(GLint)read_u32(&p); glBindTexture(t,x); break; }
        case GLCMD_ACTIVE_TEXTURE: glActiveTexture((GLenum)read_u32(&p)); break;
        case GLCMD_TEX_ENVI: { GLint t=(GLint)read_u32(&p); GLint pn=(GLint)read_u32(&p); GLint pv=(GLint)read_u32(&p); glTexEnvi(t,pn,pv); break; }
        case GLCMD_TEX_ENVFV: { GLenum t=(GLenum)read_u32(&p); GLenum pn=(GLenum)read_u32(&p);
            GLfloat params[4]={read_f32(&p),read_f32(&p),read_f32(&p),read_f32(&p)}; glTexEnvfv(t,pn,params); break; }
        case GLCMD_TEX_OFFSET: { GLenum u=(GLenum)read_u32(&p); float a=read_f32(&p); float b=read_f32(&p); glTexOffset(u,a,b); break; }
        case GLCMD_ENABLE: glEnable((GLint)read_u32(&p)); break;
        case GLCMD_DISABLE: glDisable((GLint)read_u32(&p)); break;
        case GLCMD_DEPTH_MASK: glDepthMask((GLint)read_u32(&p)); break;
        case GLCMD_CLEAR: glClear((GLint)read_u32(&p)); break;
        default: break;
        }
    }
    return (uint32_t)(p - buf);
}
```

（保留 glcmd_stream.c 里原有 `read_u32/read_f32` 和 `glcmd_replay` 的全部 case，仅把缓冲管理换成上面的 N 缓冲结构。）

- [ ] **Step 3: 更新 `main/tinygl_test.c` 的 `render_frame`**

把 `render_frame`（约 475–499 行）的 `#ifdef TGL_WASM_GAME` 块改为：

```c
#ifdef TGL_WASM_GAME
    extern volatile int tgl_multitex_tris;   /* diagnostic (ztriangle.c) */
    extern volatile int tgl_dbg_addmode;
    tgl_multitex_tris = 0;
    tgl_dbg_addmode = 0;
    uint32_t len;
    const uint8_t *buf = glcmd_frame_poll(&len);
    if (buf) {
        glcmd_replay(buf, len);
        glcmd_frame_release();
    }
#else
    /* Non-wasm fallback: native scene queue on the clear color. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0, 0, -3.5f);
    glRotatef(25, 1, 0, 0);
    glRotatef(angle_y, 0, 1, 0);
    render_queue_drain(render_cmd_draw);
#endif
    gl_flush_to_display();
```

- [ ] **Step 4: 构建验证**

Run: `idf.py build`
Expected: 编译通过。若 `wasm_game.c`/`tinygl_test.c` 引用旧 API（`glcmd_frame_len` 等）报错，全部改为新 API。

- [ ] **Step 5: 硬件验证**

烧录后：场景 30fps 正常渲染、无撕裂、无花屏、无崩溃。可在串口确认 `%d iters OK` 稳定增长（编码端无阻塞）。

- [ ] **Step 6: Commit**

```bash
git add main/glcmd_stream.c main/glcmd_stream.h main/tinygl_test.c
git commit -m "perf(glcmd): N-buffer zero-copy ping-pong (paced, no memcpy)"
```

---

### Task 3: 显示 N 帧缓冲流水线 + 后处理钩子

**Files:**
- Modify: `main/display_backend.h`（`get_buffer/flush/wait_dma` 语义）
- Modify: `main/display_st7735.c`（N fb 缓冲、旋转、按缓冲 DMA）
- Modify: `components/tinygl/src/zbuffer.c` + `components/tinygl/src/zbuffer.h`（加 `zb_set_pbuf`）
- Modify: `main/tinygl_test.c`（每帧设 pbuf、调 `gl_post_process`、zbuf 放 SRAM）
- Modify: `main/display_backend.h` / `main/display_st7735.c`（`DISPLAY_NUM_BUFFERS`）

**Interfaces:**
- Consumes: Task 2 的 `glcmd_frame_poll/release`；现有 `s_display->*`
- Produces: `void zb_set_pbuf(ZBuffer *zb, void *pbuf)`；`void gl_post_process(uint16_t *fb, int w, int h)`（no-op 钩子）；`#define DISPLAY_NUM_BUFFERS 3`

- [ ] **Step 1: 扩展 `main/display_backend.h` 语义注释**

保持函数签名不变，但明确新语义（旋转式）：
- `get_buffer()`：返回**当前渲染目标**（下一次 flush 会 DMA 它）；每次 flush 后内部推进到下一个缓冲。
- `flush()`：DMA 当前渲染目标，随后推进旋转索引。
- `wait_dma()`：等待**即将作为渲染目标的缓冲**的 DMA 完成（`DISPLAY_NUM_BUFFERS` 缓冲下稳态不阻塞）。

更新头文件注释：
```c
typedef struct {
    void   *(*init)(int width, int height, int pixel_format);
    void   (*clear)(uint16_t color);
    void   (*flush)(void);       /* DMA current render target, advance rotation */
    void   (*wait_dma)(void);    /* wait until NEXT render target's DMA done */
    void  *(*get_buffer)(void);  /* current render target (rotates per flush) */
    int    (*get_width)(void);
    int    (*get_height)(void);
} display_backend_t;
```

- [ ] **Step 2: 重写 `main/display_st7735.c` 为 N 缓冲**

替换 `s_fb` 为数组并加旋转 + 按缓冲 DMA 跟踪：

```c
#ifndef DISPLAY_NUM_BUFFERS
#define DISPLAY_NUM_BUFFERS 3   /* render / post / DMA pipeline */
#endif

static uint16_t *s_fb[DISPLAY_NUM_BUFFERS] = { 0 };
static int       s_cur = 0;                 /* current render target */
static int       s_w = 0, s_h = 0, s_fmt = 0;

#ifndef TGL_EMU_BUILD
static SemaphoreHandle_t s_dma_done_sem = NULL;
static volatile bool     s_dma_busy[DISPLAY_NUM_BUFFERS] = { false };
static volatile int      s_pending_buf = -1;   /* which buffer's DMA is in flight */
static bool IRAM_ATTR flush_ready_cb(void *ctx) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_pending_buf >= 0) s_dma_busy[s_pending_buf] = false;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}
#endif

static void *st7735_init(int w, int h, int pixel_format)
{
    s_w = w; s_h = h; s_fmt = pixel_format;
    for (int i = 0; i < DISPLAY_NUM_BUFFERS; i++) {
        s_fb[i] = (uint16_t *)heap_caps_malloc(w * h * 2,
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (!s_fb[i]) { ESP_LOGE(TAG, "fb[%d] alloc failed", i); return NULL; }
        memset(s_fb[i], 0, w * h * 2);
    }
#ifndef TGL_EMU_BUILD
    s_dma_done_sem = xSemaphoreCreateBinary();
    if (!s_dma_done_sem) { ESP_LOGE(TAG, "DMA sem failed"); return NULL; }
    hw_display_set_flush_ready_cb(flush_ready_cb, s_dma_done_sem);
#endif
    hw_display_on();
    ESP_LOGI(TAG, "ST7735 %d-fb backend: %dx%d fmt=%d", DISPLAY_NUM_BUFFERS, w, h, pixel_format);
    return s_fb[0];
}

static void st7735_wait_dma(void)
{
#ifndef TGL_EMU_BUILD
    /* Only block if the buffer we're about to render into still has a pending
     * DMA. Steady state (DMA 5.5ms vs 33ms frame, N buffers): never blocks. */
    if (s_dma_busy[s_cur]) {
        xSemaphoreTake(s_dma_done_sem, portMAX_DELAY);   /* ISR cleared s_dma_busy */
    }
#endif
}

static void st7735_flush(void)
{
    if (!s_fb[s_cur]) return;
#ifndef TGL_EMU_BUILD
    s_dma_busy[s_cur] = true;
    s_pending_buf = s_cur;
    hw_display_flush(0, 0, s_w - 1, s_h - 1, (uint8_t *)s_fb[s_cur]);
    s_cur = (s_cur + 1) % DISPLAY_NUM_BUFFERS;
#endif
}

static void *st7735_get_buffer(void)  { return s_fb[s_cur]; }
static int  st7735_get_width(void)    { return s_w; }
static int  st7735_get_height(void)   { return s_h; }
```

（`st7735_clear` 改为清 `s_fb[s_cur]`；`display_backend_t st7735_display_backend` 初始化器不变。）

- [ ] **Step 3: TinyGL 加 `zb_set_pbuf`**

在 `components/tinygl/src/zbuffer.h` 声明、`zbuffer.c` 实现：

```c
/* zbuffer.h */
void zb_set_pbuf(ZBuffer *zb, void *pbuf);
```
```c
/* zbuffer.c */
void zb_set_pbuf(ZBuffer *zb, void *pbuf)
{
    if (!zb) return;
    zb->pbuf = pbuf;
    zb->frame_buffer_allocated = 0;
}
```

- [ ] **Step 4: `main/tinygl_test.c` — 每帧设 pbuf + 后处理钩子 + zbuf 进 SRAM**

在 `gl_init` 里把 ZB 的 zbuf 放 SRAM（`gl_malloc` 改为内部 RAM——TinyGL `components/tinygl/src/memory.c`）：
```c
/* memory.c: prefer internal SRAM for TinyGL buffers (graphics is priority) */
#include "esp_heap_caps.h"
void *gl_malloc(GLint size) { return heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
```

新增后处理钩子（`tinygl_test.c` 顶部，或独立函数）：
```c
/* Post-process hook — pipeline stage between render and DMA. No-op for now;
 * future: dither / color grade / temporal effects using the retained past frame. */
__attribute__((weak)) void gl_post_process(uint16_t *fb, int w, int h)
{
    (void)fb; (void)w; (void)h;
}
```

改 `render_frame` 的 `#ifdef TGL_WASM_GAME` 块（在 replay 后、flush 前）：
```c
    uint32_t len;
    const uint8_t *buf = glcmd_frame_poll(&len);
    if (buf) {
        s_display->wait_dma();                 /* target buffer free */
        uint16_t *target = (uint16_t *)s_display->get_buffer();
        zb_set_pbuf(s_zb, target);             /* TinyGL renders here */
        glcmd_replay(buf, len);
        glcmd_frame_release();
        gl_post_process(target, s_width, s_height);
        gl_flush_to_display();                 /* DMA target + advance rotation */
    }
```

注：`gl_flush_to_display()` → `s_display->flush()` DMA 当前渲染目标（即 `get_buffer()` 返回的缓冲）；旋转在 flush 内推进。

- [ ] **Step 5: 构建验证**

Run: `idf.py build`
Expected: 编译通过。

- [ ] **Step 6: 硬件验证**

烧录后：场景正常、**无撕裂**、无花屏、无越界写（若 3 缓冲分配失败会打 `fb[i] alloc failed` 日志）。旋转 + DMA 流水线正确。

- [ ] **Step 7: Commit**

```bash
git add main/display_st7735.c main/display_backend.h main/tinygl_test.c components/tinygl/src/zbuffer.c components/tinygl/src/zbuffer.h components/tinygl/src/memory.c
git commit -m "feat(display): N-framebuffer pipeline + post-process hook, SRAM-first"
```

---

### Task 4: mem_mgr 组件化 + 收尾

**Files:**
- Create: `main/mem_mgr.h`, `main/mem_mgr.c`
- Modify: `main/CMakeLists.txt`（SRCS 加 `mem_mgr.c`）
- Modify: `main/wasm_game.c`（分配器改用 `mem_bulk_alloc`）
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Produces:
  - `void *mem_hot_alloc(size_t size)` → SRAM（`MALLOC_CAP_INTERNAL|8BIT`），预算记账
  - `void *mem_bulk_alloc(size_t size)` → PSRAM（`MALLOC_CAP_SPIRAM|8BIT`），预算记账
  - `void *mem_bulk_realloc(void *ptr, size_t size)` / `void mem_bulk_free(void *ptr)`
  - `void mem_mgr_dump(void)` → 打印各池已分配/预算

- [ ] **Step 1: 新建 `main/mem_mgr.h`**

```c
#pragma once
#include <stddef.h>
/* Device memory manager — routes allocations by "hotness" intent.
 * SRAM (hot, graphics engine) is budget-limited; PSRAM (bulk) is plentiful.
 * The wasm game never sees this — it only uses standard wasm memory. */
void *mem_hot_alloc(size_t size);
void *mem_bulk_alloc(size_t size);
void *mem_bulk_realloc(void *ptr, size_t size);
void  mem_bulk_free(void *ptr);
void  mem_mgr_dump(void);
```

- [ ] **Step 2: 新建 `main/mem_mgr.c`**

```c
#include "mem_mgr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>

static const char *TAG = "mem_mgr";

/* SRAM hot budget (bytes). Graphics engine consumes ~176KB; remaining ~40KB
 * is the budget for other hot allocations. */
#define MEM_SRAM_HOT_BUDGET (40 * 1024)

static volatile size_t s_sram_hot_used = 0;
static volatile size_t s_psram_used    = 0;

void *mem_hot_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) {
        size_t u = s_sram_hot_used + size;
        if (u <= MEM_SRAM_HOT_BUDGET) s_sram_hot_used = u;
        else ESP_LOGW(TAG, "hot budget exceeded: %u > %u", (unsigned)u, (unsigned)MEM_SRAM_HOT_BUDGET);
    }
    return p;
}

void *mem_bulk_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) s_psram_used += size;
    return p;
}

void *mem_bulk_realloc(void *ptr, size_t size)
{
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) s_psram_used += size;   /* approximate (no shrink tracking) */
    return p;
}

void mem_bulk_free(void *ptr) { heap_caps_free(ptr); }

void mem_mgr_dump(void)
{
    ESP_LOGI(TAG, "sram_hot=%u/%u  psram=%u",
             (unsigned)s_sram_hot_used, (unsigned)MEM_SRAM_HOT_BUDGET, (unsigned)s_psram_used);
}
```

- [ ] **Step 3: `main/wasm_game.c` 分配器改用 mem_mgr**

把 `wamr_malloc/wamr_realloc/wamr_free` 改为调用 mem_mgr：
```c
#include "mem_mgr.h"
static void *wamr_malloc(size_t size)   { return mem_bulk_alloc(size); }
static void *wamr_realloc(void *ptr, size_t size) { return mem_bulk_realloc(ptr, size); }
static void  wamr_free(void *ptr)       { mem_bulk_free(ptr); }
```

- [ ] **Step 4: `main/CMakeLists.txt` SRCS 加 `"mem_mgr.c"`**

- [ ] **Step 5: 构建 + 硬件验证**

Run: `idf.py build`
烧录后 `mem_mgr` 日志（可在 `wasm_game_task` 里 `mem_mgr_dump()` 打印一次）确认 WAMR 分配走 PSRAM、SRAM 余量充足。游戏正常。

- [ ] **Step 6: 更新 `.superpowers/sdd/progress.md`**

追加：
```markdown
- [x] wasm3 → WAMR 引擎替换 + 图形引擎 SRAM 优先内存设计（spec/plan: docs/superpowers/specs/2026-08-03-wamr-engine-switch-design.md）
  - WAMR fast interp（有界原生栈）跑同一份 wasm_game.wasm，22 个 GL 原生 → glcmd_stream，渲染零改动
  - glcmd N 缓冲（2/3 可配）零拷贝 ping-pong + paced，省每帧 memcpy
  - 显示 N 帧缓冲流水线（默认 3）+ 过去帧 + 后处理 no-op 钩子
  - mem_mgr：图形引擎 SRAM 优先，WAMR 全 PSRAM
  - 删 wasm3 组件 + 还原 128KB PSRAM 栈 hack
  - 待办：金属立方体反光无效果（独立渲染问题，继续排查）
```

- [ ] **Step 7: 全量验证 + Commit**

Run: `idf.py build`（干净）+ 烧录确认 30fps 场景正常、日志符合预期。
```bash
git add -A && git commit -m "refactor(mem): mem_mgr hot/bulk allocator, WAMR→PSRAM"
```

---

## 自审对照（spec → task）

| spec 章节 | 落实任务 |
|-----------|----------|
| §1 引擎替换（wasm_game.c、任务改名、删 wasm3） | Task 1 |
| §1 wasm 构建不变 | Task 1 Step 6（不碰 CMake wasm 生成） |
| §2 N 缓冲零拷贝 paced 同步 | Task 2 |
| §3 N-fb 显示流水线 + 过去帧 + 后处理 no-op | Task 3 |
| §3 TinyGL pbuf 切换 | Task 3 Step 3 |
| §4 mem_mgr（WAMR→PSRAM、图形→SRAM） | Task 4 |
| §4 纹理 flash / 不缓存 | 不做（保持现状） |
| §5 清理（删 wasm3、还原栈、删配置） | Task 1 Step 3–6 |
| 明确不做：脏矩形、LLM、后处理效果、纹理缓存、金属反光 | 均不实现 |

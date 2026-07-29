/**
 * TinyGL Render Host API for WAMR wasm games.
 *
 * Registers native functions that wasm bytecode can call to submit
 * draw commands to the render queue (consumed by core 1).
 *
 * Functions exposed to wasm:
 *   render_clear(r, g, b)
 *   render_submit_cube(x, y, z, size, rx, ry, tex_id)
 *   physics_step(dt)
 *   physics_spawn(x, y, z, size)
 *   physics_body_count() → int
 *   physics_body_pos(idx, *x, *y, *z)
 */
#include "wasm_export.h"
#include "esp_log.h"

#include "render_queue.h"
#include "tinygl_physics.h"

static const char *TAG = "render_api";

/* ── wasm-callable wrappers ─────────────────────────────────────────────── */

/* render_clear(r, g, b) — no-op; skybox + clear handled by render loop */
static void wasm_render_clear(wasm_exec_env_t env, int r, int g, int b)
{
    (void)env; (void)r; (void)g; (void)b;
    /* clear is done by the render loop on core 1; wasm doesn't need this */
}

/* render_submit_cube(x, y, z, size, rx, ry, tex_id) */
static void wasm_render_submit_cube(wasm_exec_env_t env,
                                     float x, float y, float z,
                                     float size, float rx, float ry,
                                     int tex_id)
{
    (void)env;
    if (tex_id < 1 || tex_id > 4) tex_id = 1;
    render_queue_push((render_cmd_t){
        .x = x, .y = y, .z = z,
        .size = size,
        .rx = rx, .ry = ry,
        .tex_id = (unsigned int)tex_id,
    });
}

/* physics_step(dt) */
static void wasm_physics_step(wasm_exec_env_t env, float dt)
{
    (void)env;
    physics_step(dt);
}

/* physics_spawn(x, y, z, size) → body_id */
static int wasm_physics_spawn(wasm_exec_env_t env,
                               float x, float y, float z, float size)
{
    (void)env;
    return physics_spawn(x, y, z, size);
}

/* physics_body_count() → int */
static int wasm_physics_body_count(wasm_exec_env_t env)
{
    (void)env;
    int count = 0;
    for (int i = 0; i < MAX_BODIES; i++) {
        if (s_bodies[i].active) count++;
    }
    return count;
}

/* physics_body_pos(idx, *x, *y, *z) */
static void wasm_physics_body_pos(wasm_exec_env_t env,
                                   int idx,
                                   uint32_t x_ptr, uint32_t y_ptr, uint32_t z_ptr)
{
    (void)env;
    body_t *b = physics_get(idx);
    if (!b || !b->active) return;

    /* Write through WAMR memory */
    if (x_ptr) {
        wasm_runtime_validate_app_str_addr((wasm_module_inst_t)
            wasm_runtime_get_module_inst(env), x_ptr, 4);
        *(float *)wasm_runtime_addr_app_to_native(
            wasm_runtime_get_module_inst(env), x_ptr) = b->x;
    }
    if (y_ptr) {
        wasm_runtime_validate_app_str_addr((wasm_module_inst_t)
            wasm_runtime_get_module_inst(env), y_ptr, 4);
        *(float *)wasm_runtime_addr_app_to_native(
            wasm_runtime_get_module_inst(env), y_ptr) = b->y;
    }
    if (z_ptr) {
        wasm_runtime_validate_app_str_addr((wasm_module_inst_t)
            wasm_runtime_get_module_inst(env), z_ptr, 4);
        *(float *)wasm_runtime_addr_app_to_native(
            wasm_runtime_get_module_inst(env), z_ptr) = b->z;
    }
}

/* ── registration ───────────────────────────────────────────────────────── */

#define REG_WASM(name, func, sig) do {                                  \
    NativeSymbol ns;                                                    \
    ns.module = NULL;                                                   \
    ns.symbol = name;                                                   \
    ns.func_ptr = (void *)func;                                         \
    ns.signature = sig;                                                 \
    ns.attachment = NULL;                                               \
    if (!wasm_runtime_register_natives("env", &ns, 1)) {                \
        ESP_LOGE(TAG, "Failed to register %s", name);                   \
        return false;                                                   \
    }                                                                   \
} while(0)

bool render_api_register(void)
{
    REG_WASM("render_clear",          wasm_render_clear,          "(iii)");
    REG_WASM("render_submit_cube",    wasm_render_submit_cube,    "(fffffffiii)");
    REG_WASM("physics_step",          wasm_physics_step,          "(f)");
    REG_WASM("physics_spawn",         wasm_physics_spawn,         "(ffff)i");
    REG_WASM("physics_body_count",    wasm_physics_body_count,    "()i");
    REG_WASM("physics_body_pos",      wasm_physics_body_pos,      "(iiii)");

    ESP_LOGI(TAG, "Render + physics Host API registered (6 functions)");
    return true;
}
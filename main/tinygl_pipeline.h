/**
 * TinyGL rendering pipeline declarations for the Xiaomiao ESP32 handheld.
 *
 * Minimal header — only exposes what main.c and the WASM host need.
 * The full implementation is in tinygl_test.c.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** One-time GL initialisation. Must be called before any draw call. */
int gl_init(int width, int height);

/** Render one complete frame (skybox + scene + flush). */
void render_frame(float angle_y);

/** Render loop task function (pinned to core 1). */
void tinygl_render_task(void *arg);

/* ── Texture IDs (bound at gl_init time) ───────────────────────────────── */
#define TEX_CERAMIC  1
#define TEX_CHECKER  2
#define TEX_BRICK    3
#define TEX_GRID     4

#ifdef __cplusplus
}
#endif
/**
 * TinyGL rendering pipeline for the Xiaomiao ESP32 handheld.
 *
 * Thin abstraction over TinyGL's fixed-function OpenGL 1.x pipeline:
 *   - gl_init() initialises the Z-buffer, textures, and lighting
 *   - draw_skybox() / draw_textured_cube() are geometry helpers
 *   - render_frame() executes one frame: skybox → scene → flush
 *
 * All GL state lives in TinyGL's global context; this file is a
 * convenience wrapper, not a full scene-graph engine.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/** One-time GL initialisation.  Must be called before any draw call. */
int gl_init(int width, int height);

/** Render one complete frame (skybox + scene + flush). */
void render_frame(float angle_y);

/* ── Geometry helpers ──────────────────────────────────────────────────── */

/** Draw a single textured cube at (x,y,z) with local rotation (rx,ry)
 *  and the given texture id (1-4).  Normals are outward for lighting. */
void draw_textured_cube(float x, float y, float z, float size,
                        float rx, float ry, unsigned int tex_id);

/** Draw the 6-face skybox (centred on the camera, no depth-write). */
void draw_skybox(void);

/* ── Runtime controls ──────────────────────────────────────────────────── */

/** Pause/resume the render loop (cross-task volatile). */
extern volatile int tinygl_render_paused;

/** Number of cubes drawn per frame (1-16). */
extern volatile int tinygl_cube_count;

/** Enable/disable physics-driven cube placement. */
extern volatile int tinygl_physics_mode;

/** Enable/disable the skybox. */
extern int tinygl_skybox_enabled;

/** Texture IDs (bound at gl_init time). */
#define TEX_CERAMIC  1
#define TEX_CHECKER  2
#define TEX_BRICK    3
#define TEX_GRID     4

#ifdef __cplusplus
}
#endif
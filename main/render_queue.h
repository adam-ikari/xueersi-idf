/**
 * Cross-core render command queue for the Xiaomiao TinyGL pipeline.
 *
 * Core 0 (game logic / physics) pushes render commands; core 1 (render)
 * drains them each frame. A single-producer single-consumer ring buffer
 * avoids locks — only a volatile head/tail with memory barriers.
 *
 * Each command is a flat struct of floats + texture id — small enough
 * that a 32-slot ring buffer fits in a few hundred bytes.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RENDER_QUEUE_SIZE 32

typedef struct {
    float x, y, z;          /* world position (center) */
    float rx, ry;           /* rotation angles (degrees) */
    float size;             /* cube half-size * 2 */
    unsigned int tex_id;    /* texture id (1-4) */
} render_cmd_t;

/** Non-blocking push from core 0. Returns false if queue is full. */
bool render_queue_push(render_cmd_t cmd);

/** Pop all pending commands from core 1. Returns number of items consumed.
 *  The callback is invoked once per command with the popped data. */
int render_queue_drain(void (*draw_fn)(const render_cmd_t *cmd));

#ifdef __cplusplus
}
#endif
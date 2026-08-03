/**
 * TinyGL GL command stream — wasm3 → native bridge.
 *
 * The wasm3 game on core 0 calls GL API lookalikes (glBegin, glVertex3f, ...).
 * Each call is serialized into a byte stream by the wasm3 host wrappers. Core 1
 * drains the latest frame and replays the calls against the real TinyGL context.
 *
 * Command format: [opcode:1B] [payload:N bytes] (little-endian).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opcodes ────────────────────────────────────────────── */

enum {
    GLCMD_NOP = 0,

    /* Begin/End */
    GLCMD_BEGIN,            // u32 mode
    GLCMD_END,

    /* Vertices */
    GLCMD_VERTEX3F,         // f32 x, y, z
    GLCMD_COLOR3F,          // f32 r, g, b
    GLCMD_NORMAL3F,         // f32 nx, ny, nz
    GLCMD_TEXCOORD2F,       // f32 s, t

    /* Matrix */
    GLCMD_MATRIX_MODE,      // u32 mode
    GLCMD_LOAD_IDENTITY,
    GLCMD_PUSH_MATRIX,
    GLCMD_POP_MATRIX,
    GLCMD_ROTATEF,          // f32 angle, x, y, z
    GLCMD_TRANSLATEF,       // f32 x, y, z

    /* Texture */
    GLCMD_BIND_TEXTURE,     // u32 target, u32 texture_id
    GLCMD_ACTIVE_TEXTURE,   // u32 texture_unit (GL_TEXTURE0-3)
    GLCMD_TEX_ENVI,         // u32 target, pname, param
    GLCMD_TEX_ENVFV,        // u32 target, pname, f32[4] params
    GLCMD_TEX_OFFSET,       // u32 unit, f32 u, f32 v

    /* State */
    GLCMD_ENABLE,           // u32 cap
    GLCMD_DISABLE,          // u32 cap
    GLCMD_DEPTH_MASK,       // u32 flag
    GLCMD_CLEAR,            // u32 mask

    GLCMD_COUNT
};

/* ── Stream buffer ──────────────────────────────────────── */

#define GLCMD_BUFFER_SIZE 8192  /* 8 KB — one frame of GL calls */

/* ── Encoder (wasm3 host side, core 0) ──────────────────── */

void glcmd_begin_frame(void);
bool glcmd_u8(uint8_t v);
bool glcmd_u32(uint32_t v);
bool glcmd_f32(float v);

/** Publish the accumulated frame to core 1 (single-slot latest-wins). */
void glcmd_publish(void);
/** Length of the latest published frame (0 = none). */
uint32_t glcmd_frame_len(void);
const uint8_t *glcmd_frame_buf(void);
/** Mark the current frame consumed (core 1). */
void glcmd_frame_clear(void);

/* ── Decoder (native side, core 1) ──────────────────────── */

/** Replay a GL command stream against the active TinyGL context.
 *  @param buf   Pointer to byte stream.
 *  @param len   Number of bytes to replay.
 *  @return Number of bytes consumed. */
uint32_t glcmd_replay(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

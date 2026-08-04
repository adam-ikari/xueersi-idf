/**
 * TinyGL GL command stream — wasm → native bridge.
 *
 * The wasm game on core 0 calls GL API lookalikes (glBegin, glVertex3f, ...).
 * Each call is serialized into a byte stream by the wasm host wrappers. Core 1
 * drains each published frame and replays the calls against the real TinyGL
 * context (N-buffer zero-copy ping-pong; paced, no frame dropping).
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
/* Number of ping-pong frame buffers (2 = paced, 3 = extra jitter slack). */
#define GLCMD_NUM_BUFFERS 2

/* ── Encoder (wasm host side, core 0) ──────────────────── */

void glcmd_begin_frame(void);          /* wait for a free buffer, reset pos */
void glcmd_publish(void);              /* publish current buffer, advance */
bool glcmd_u8(uint8_t v);
bool glcmd_u32(uint32_t v);
bool glcmd_f32(float v);

/* ── Decoder (native side, core 1) ──────────────────────── */

/** Poll the next unconsumed frame (zero-copy; buffer owned by core 0 until
 *  release). Returns NULL if this cycle's buffer has no frame.
 *  @param out_len  Receives the frame length in bytes.
 *  @return Pointer to the frame, or NULL. */
const uint8_t *glcmd_frame_poll(uint32_t *out_len);
/** Mark the polled frame consumed and advance to the next buffer (core 1). */
void glcmd_frame_release(void);

/** Replay a GL command stream against the active TinyGL context.
 *  @param buf   Pointer to byte stream.
 *  @param len   Number of bytes to replay.
 *  @return Number of bytes consumed. */
uint32_t glcmd_replay(const uint8_t *buf, uint32_t len);

/** Push a key event into the WASM game's input ring buffer.
 *  Called from the render task (core 1) — thread-safe.
 *  @param btn_idx  0=UP 1=DOWN 2=LEFT 3=RIGHT 4=A 5=B
 *  @param down     true = down edge, false = up edge */
void wasm_key_push(int btn_idx, int down);

#ifdef __cplusplus
}
#endif

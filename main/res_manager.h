/**
 * Resource manager — pixel-data layer, decoupled from wasm and from rendering.
 *
 * wasm never sees pixel data. It requests textures by NAME (a stable contract,
 * e.g. "metal"); the host resolves the name here and forwards a resolved
 * read-only DROM data pointer + w/h down the GL command stream to core 1.
 *
 * Data source is irrelevant to the contract: today the 128x128 RGB888 arrays
 * are compile-time headers in flash; a future FATFS/TF backend can point
 * `data` at a lazily-materialised buffer (or NULL until materialised) with
 * zero wasm changes.
 */
#pragma once

#include <stdint.h>

typedef struct {
    const char    *name;      /* stable contract name, e.g. "metal" */
    const uint8_t *data;      /* read-only pixel data (DROM, core-safe) */
    uint32_t       w, h;      /* dimensions (128x128 today) */
    uint32_t       channels;  /* 3 (RGB888) — satisfies glTexImage2D checks */
} tex_res_t;

/* Name-addressed lookup; NULL for unknown name. */
const tex_res_t *res_lookup(const char *name);
/* Index-addressed lookup (bounds-checked); NULL past the end. */
const tex_res_t *res_at(int idx);
/* Number of registered resources. */
int res_count(void);

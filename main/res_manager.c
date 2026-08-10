/**
 * Resource manager — catalog of all game textures.
 *
 * This file is the SINGLE translation unit that includes the generated
 * `texture_*.h` headers (they are `static const`, internal linkage — a second
 * includer would double the ~49KB-per-texture .rodata). It is the only bridge
 * between resource data and the rest of the system: everyone else addresses
 * resources by name through res_manager.h.
 */
#include "res_manager.h"

#include "texture_sky.h"
#include "texture_horizon.h"
#include "texture_sand.h"
#include "texture_metal.h"
#include "texture_reflect.h"
#include "texture_brick.h"

#include <string.h>

static const tex_res_t s_res[] = {
    { "sky",     texture_sky_data,     128, 128, 3 },
    { "horizon", texture_horizon_data, 128, 128, 3 },
    { "sand",    texture_sand_data,    128, 128, 3 },
    { "metal",   texture_metal_data,   128, 128, 3 },
    { "reflect", texture_reflect_data, 128, 128, 3 },
    { "brick",   texture_brick_data,   128, 128, 3 },
};

const tex_res_t *res_lookup(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < (int)(sizeof(s_res) / sizeof(s_res[0])); i++)
        if (strcmp(s_res[i].name, name) == 0) return &s_res[i];
    return NULL;
}

const tex_res_t *res_at(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(s_res) / sizeof(s_res[0]))) return NULL;
    return &s_res[idx];
}

int res_count(void)
{
    return (int)(sizeof(s_res) / sizeof(s_res[0]));
}

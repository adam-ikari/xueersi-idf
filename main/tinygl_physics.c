/**
 * Minimal rigid-body physics for the TinyGL demo scene.
 *
 * All active bodies are stored in the globally-visible `s_bodies[]` array
 * (non-static so core 1 can read positions for rendering while core 0 runs
 * the simulation). Access to the body array is NOT locked — the renderer
 * reads positions atomically (floats are 32-bit aligned on ESP32, single-
 * store atomic on Xtensa) and the physics task writes them with no read-
 * modify-write cycles on those fields.
 */
#include "tinygl_physics.h"
#include <math.h>
#include <string.h>

body_t s_bodies[MAX_BODIES];  /* globally visible for cross-core rendering */
static float  s_time;          /* accumulated simulation time */

void physics_init(void)
{
    memset(s_bodies, 0, sizeof(s_bodies));
    s_time = 0.0f;
}

int physics_spawn(float x, float y, float z, float hs)
{
    for (int i = 0; i < MAX_BODIES; i++) {
        if (!s_bodies[i].active) {
            s_bodies[i] = (body_t){
                .x = x, .y = y, .z = z,
                .vx = 0, .vy = 0, .vz = 0,
                .hs = hs, .mass = 1.0f,
                .active = true, .grounded = false,
            };
            return i;
        }
    }
    return -1;
}

body_t *physics_get(int id)
{
    if (id < 0 || id >= MAX_BODIES) return NULL;
    return &s_bodies[id];
}

static void collide_ground(body_t *b)
{
    float floor_y = GROUND_Y + b->hs;   /* cube bottom hits ground */
    if (b->y <= floor_y) {
        b->y = floor_y;
        if (b->vy < 0) {
            b->vy = -b->vy * RESTITUTION;
            if (b->vy > -0.3f) {        /* settle when slow */
                b->vy = 0;
                b->grounded = true;
            }
        }
    } else {
        b->grounded = false;
    }
}

static void collide_walls(body_t *b)
{
    float lim_x = WALL_X - b->hs;
    float lim_z = WALL_Z - b->hs;
    if (b->x >  lim_x) { b->x =  lim_x; b->vx = -b->vx * RESTITUTION; }
    if (b->x < -lim_x) { b->x = -lim_x; b->vx = -b->vx * RESTITUTION; }
    if (b->z >  lim_z) { b->z =  lim_z; b->vz = -b->vz * RESTITUTION; }
    if (b->z < -lim_z) { b->z = -lim_z; b->vz = -b->vz * RESTITUTION; }
}

void physics_step(float dt)
{
    s_time += dt;
    for (int i = 0; i < MAX_BODIES; i++) {
        body_t *b = &s_bodies[i];
        if (!b->active) continue;

        /* gravity (skip if resting on ground with no upward velocity) */
        if (!b->grounded) {
            b->vy += GRAVITY * dt;
        }

        /* integrate */
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        b->z += b->vz * dt;

        /* ground + wall collisions */
        collide_ground(b);
        collide_walls(b);
    }
}

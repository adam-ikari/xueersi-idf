/**
 * Minimal rigid-body physics for the TinyGL demo scene.
 *
 * Gravity + velocity integration + ground-plane collision (AABB-ish: each
 * body is treated as an axis-aligned cube of half-size `hs`). No rotations
 * in the physics (visual rotation is separate). Designed for a handful of
 * bodies on ESP32 — O(n) per step, no broadphase.
 */
#ifndef TINYGL_PHYSICS_H
#define TINYGL_PHYSICS_H

#include <stdbool.h>

#define MAX_BODIES 16

typedef struct {
    float x, y, z;          /* position (center) */
    float vx, vy, vz;       /* velocity */
    float hs;               /* half-size (cube) */
    float mass;             /* mass (for future collisions) */
    bool  active;           /* in use? */
    bool  grounded;         /* resting on ground? */
} body_t;

/* World bounds: ground at y=GROUND_Y, walls at ±WALL_X/Z. */
#define GROUND_Y   (-3.0f)
#define WALL_X     ( 4.0f)
#define WALL_Z     ( 4.0f)
#define GRAVITY    (-9.8f)
#define RESTITUTION (0.45f)   /* bounciness on ground hit */

void physics_init(void);
int  physics_spawn(float x, float y, float z, float hs);  /* returns body id, -1 if full */
body_t *physics_get(int id);
void physics_step(float dt);  /* advance all active bodies by dt seconds */

/* Direct body array access for cross-core rendering (no lock — floats
 * are single-store atomic on Xtensa). Core 1 reads position fields;
 * core 0 writes via physics_step. */
extern body_t s_bodies[MAX_BODIES];

#endif /* TINYGL_PHYSICS_H */

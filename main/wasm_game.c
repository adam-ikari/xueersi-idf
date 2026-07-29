/**
 * WASM game demo for Xiaomiao — rotating textured cube with gravity bounce.
 *
 * Invokes native Host APIs (render_*, physics_*) registered by render_api.c.
 * The wasm module runs on core 0; core 1 renders whatever is in the render queue.
 *
 * Build: wat2wasm game.wat -o game.wasm
 * Or: clang --target=wasm32 -O3 -nostdlib -o game.wasm game.c
 */

/* Native host functions (imported from "env") */
extern void render_submit_cube(float x, float y, float z,
                                float size, float rx, float ry,
                                int tex_id);
extern void physics_step(float dt);
extern int  physics_spawn(float x, float y, float z, float size);
extern int  physics_body_count(void);
extern void physics_body_pos(int idx, uint32_t x_ptr, uint32_t y_ptr, uint32_t z_ptr);

/* ── Simple demo: rotating cube with gravity ───────────────────────────── */

static float angle = 0.0f;
static int   spawned = 0;

void game_init(void)
{
    /* Spawn one cube slightly above the ground */
    physics_spawn(0.0f, 1.0f, 0.0f, 0.8f);
    spawned = 1;
}

/* Called every frame (target ~30 Hz) by the host game loop */
void game_update(void)
{
    angle += 2.0f;
    if (angle >= 360.0f) angle -= 360.0f;

    /* Physics step */
    physics_step(0.033f);

    /* If no bodies left, spawn a new one */
    if (physics_body_count() == 0) {
        physics_spawn(0.0f, 3.0f, 0.0f, 0.8f);
    }

    /* Submit the first active body for rendering */
    float x = 0.0f, y = 0.0f, z = 0.0f;
    physics_body_pos(0, (uint32_t)&x, (uint32_t)&y, (uint32_t)&z);

    /* Rotate around y-axis by angle for visual effect */
    render_submit_cube(x, y, z, 1.6f, 0.0f, angle, 1);  /* tex_id=1 ceramic */

    /* Static reference cube at origin */
    render_submit_cube(0.0f, -2.0f, 0.0f, 1.0f, angle * 0.5f, angle * 0.3f, 2);
}
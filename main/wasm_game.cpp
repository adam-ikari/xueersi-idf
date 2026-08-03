// wasm_game.cpp — Xiaomiao 3D game, submitted entirely through GL APIs.
//
// Runs on core 0 via the wasm3 interpreter. It is the sole author of every
// rendered scene: it calls GL lookalikes (glBegin / glVertex3f / ...), which
// the host encodes into a command stream that core 1 replays against TinyGL.
// Textures are referenced by integer NAME only (glBindTexture) — no texture
// data crosses the API.
//
// Build:
//   clang++ --target=wasm32 -O3 -nostdlib -c wasm_game.cpp -o wasm_game.o
//   wasm-ld --no-entry --export-all --allow-undefined \
//     --initial-memory=131072 --max-memory=131072 wasm_game.o -o wasm_game.wasm

#define IMPORT(rt, name, ...) \
    extern "C" __attribute__((import_module("env"), import_name(#name))) rt name(__VA_ARGS__);

IMPORT(void, glBegin, int mode);
IMPORT(void, glEnd);
IMPORT(void, glVertex3f, float x, float y, float z);
IMPORT(void, glColor3f, float r, float g, float b);
IMPORT(void, glNormal3f, float x, float y, float z);
IMPORT(void, glTexCoord2f, float s, float t);
IMPORT(void, glMatrixMode, int mode);
IMPORT(void, glLoadIdentity);
IMPORT(void, glPushMatrix);
IMPORT(void, glPopMatrix);
IMPORT(void, glRotatef, float a, float x, float y, float z);
IMPORT(void, glTranslatef, float x, float y, float z);
IMPORT(void, glBindTexture, int target, int tex);
IMPORT(void, glActiveTexture, int unit);
IMPORT(void, glTexEnvi, int target, int pname, int param);
IMPORT(void, glTexEnvfv, int target, int pname, float v0, float v1, float v2, float v3);
IMPORT(void, glTexOffset, int unit, float u, float v);
IMPORT(void, glEnable, int cap);
IMPORT(void, glDisable, int cap);
IMPORT(void, glDepthMask, int flag);
IMPORT(void, glClear, int mask);
IMPORT(void, glFlush);

// ── GL constants ────────────────────────────────────────
enum {
    GL_QUADS             = 0x0007,
    GL_DEPTH_TEST        = 0x0B71,
    GL_CULL_FACE         = 0x0B44,
    GL_LIGHTING          = 0x0B50,
    GL_LIGHT0            = 0x4000,
    GL_MODELVIEW         = 0x1700,
    GL_TEXTURE0          = 0x84C0,
    GL_TEXTURE1          = 0x84C1,
    GL_TEXTURE2          = 0x84C2,
    GL_TEXTURE_ENV       = 0x2300,
    GL_TEXTURE_ENV_MODE  = 0x2200,
    GL_TEXTURE_ENV_COLOR = 0x2201,
    GL_TEXTURE_2D        = 0x0DE1,
    GL_ADD               = 0x0104,
    GL_REPLACE           = 0x1E01,
    GL_COLOR_BUFFER_BIT  = 0x00004000,
    GL_DEPTH_BUFFER_BIT  = 0x00000100,
};

// Texture names (firmware-integrated; integer names, no data in the API)
enum {
    TEX_CHECKER  = 2,
    TEX_SKY      = 5,
    TEX_SAND     = 6,
    TEX_HORIZON  = 7,
    TEX_METAL    = 8,
    TEX_SPECULAR = 9,
    TEX_REFLECT  = 10,
};

// ── Global state ────────────────────────────────────────
static float s_angle = 0.0f;      /* cube rotation (fast) */
static float s_sky_angle = 0.0f;  /* skybox rotation (slower) */
static float s_scroll_v = 0.0f;   /* vertical reflection drift */

// ── Draw a cube of half-size hs, centred at origin ─────
// Per-vertex ENVIRONMENT-MAPPED reflection: each vertex reflects the view
// direction across the face normal (R = I - 2(N·I)N) and maps R to the
// environment texture — top faces sample sky, bottom faces sand, sides the
// horizon. This is what makes the surface read as polished metal rather than
// a flat-textured box. All texture units share these coords.
static void cube(float hs)
{
    // Viewer in the cube's local frame (fixed, simulated reflection).
    static const float EYE[3] = { 0, 0, 5 };
    struct Face { float n[3]; float v[4][3]; };
    static const Face F[6] = {
        { { 0, 0, 1}, { {-hs,-hs, hs}, { hs,-hs, hs}, { hs, hs, hs}, {-hs, hs, hs} } },
        { { 0, 0,-1}, { { hs,-hs,-hs}, {-hs,-hs,-hs}, {-hs, hs,-hs}, { hs, hs,-hs} } },
        { { 0, 1, 0}, { {-hs, hs, hs}, { hs, hs, hs}, { hs, hs,-hs}, {-hs, hs,-hs} } },
        { { 0,-1, 0}, { {-hs,-hs,-hs}, { hs,-hs,-hs}, { hs,-hs, hs}, {-hs,-hs, hs} } },
        { { 1, 0, 0}, { { hs,-hs, hs}, { hs,-hs,-hs}, { hs, hs,-hs}, { hs, hs, hs} } },
        { {-1, 0, 0}, { {-hs,-hs,-hs}, {-hs,-hs, hs}, {-hs, hs, hs}, {-hs, hs,-hs} } },
    };
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            float vx = F[f].v[k][0], vy = F[f].v[k][1], vz = F[f].v[k][2];
            // view ray: from surface point toward the viewer
            float ix = EYE[0] - vx, iy = EYE[1] - vy, iz = EYE[2] - vz;
            float il = __builtin_sqrtf(ix*ix + iy*iy + iz*iz);
            if (il > 0.001f) { ix /= il; iy /= il; iz /= il; }
            // reflect across the face normal
            float nd = F[f].n[0]*ix + F[f].n[1]*iy + F[f].n[2]*iz;
            float rx = ix - 2*nd*F[f].n[0];
            float ry = iy - 2*nd*F[f].n[1];
            // sphere-map: s from Rx, t from Ry (up = sky, down = sand)
            float s = (rx + 1) * 0.5f;
            float t = (ry + 1) * 0.5f;
            if (s < 0) s = 0; else if (s > 1) s = 1;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            glNormal3f(F[f].n[0], F[f].n[1], F[f].n[2]);
            glTexCoord2f(s, t);
            glVertex3f(vx, vy, vz);
        }
    }
    glEnd();
}

// ── Skybox face: subdiv x subdiv quads over a plane ────
static void skybox_face(float x0, float y0, float z0,
                        float ax, float ay, float az,
                        float bx, float by, float bz,
                        int tex)
{
    const int subdiv = 2;
    const float step = 1.0f / subdiv;
    glBindTexture(GL_TEXTURE_2D, tex);
    for (int i = 0; i < subdiv; i++) {
        for (int j = 0; j < subdiv; j++) {
            float u0 = i * step, u1 = (i + 1) * step;
            float v0 = j * step, v1 = (j + 1) * step;
            glBegin(GL_QUADS);
            glTexCoord2f(u0, v0); glVertex3f(x0 + ax*u0 + bx*v0, y0 + ay*u0 + by*v0, z0 + az*u0 + bz*v0);
            glTexCoord2f(u1, v0); glVertex3f(x0 + ax*u1 + bx*v0, y0 + ay*u1 + by*v0, z0 + az*u1 + bz*v0);
            glTexCoord2f(u1, v1); glVertex3f(x0 + ax*u1 + bx*v1, y0 + ay*u1 + by*v1, z0 + az*u1 + bz*v1);
            glTexCoord2f(u0, v1); glVertex3f(x0 + ax*u0 + bx*v1, y0 + ay*u0 + by*v1, z0 + az*u0 + bz*v1);
            glEnd();
        }
    }
}

// ── Desert skybox: sky above, sand below, horizon sides ─
static void draw_skybox(void)
{
    const float s = 15.0f;
    glDepthMask(0);
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);

    /* Each side face maps texture v along the WORLD Y (up), so the horizon's
     * sand sits at the face bottom and sky at the top. */
    /* +X */ skybox_face( s,-s,-s,  0, 0, 2*s,  0, 2*s, 0, TEX_HORIZON);
    /* -X */ skybox_face(-s,-s, s,  0, 0,-2*s,  0, 2*s, 0, TEX_HORIZON);
    /* +Y */ skybox_face(-s, s, s,  2*s, 0, 0,  0, 0,-2*s, TEX_SKY);
    /* -Y */ skybox_face(-s,-s,-s,  2*s, 0, 0,  0, 0, 2*s, TEX_SAND);
    /* +Z */ skybox_face(-s,-s, s,  2*s, 0, 0,  0, 2*s, 0,  TEX_HORIZON);
    /* -Z */ skybox_face( s,-s,-s, -2*s, 0, 0,  0, 2*s, 0,  TEX_HORIZON);

    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glDepthMask(1);
}

// ── Metal cube: 3-layer additive multi-texture ─────────
static void draw_metal_cube(float hs)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, TEX_METAL);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, TEX_REFLECT);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0.8f, 0.8f, 0.8f, 1.0f);
    /* Reflection environment offset computed from the CAMERA yaw (s_angle)
     * relative to the SKYBOX yaw (s_sky_angle), scaled so the flow is clearly
     * visible (a few texels per frame). */
    glTexOffset(GL_TEXTURE1, 3.0f * (s_angle - s_sky_angle) / 360.0f, s_scroll_v);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, TEX_SPECULAR);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0.9f, 0.9f, 0.9f, 1.0f);

    glActiveTexture(GL_TEXTURE0);
    glPushMatrix();
    /* Tumble about two axes so all faces/reflection are visible. */
    glRotatef(s_angle, 0, 1, 0);
    glRotatef(s_angle * 0.6f, 1, 0, 0);
    cube(hs);
    glPopMatrix();

    /* Reset units 1+ to REPLACE so the small cube isn't multi-textured. */
    glActiveTexture(GL_TEXTURE1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glActiveTexture(GL_TEXTURE2);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glActiveTexture(GL_TEXTURE0);
}

// ── game_init ──────────────────────────────────────────
extern "C" void game_init(void)
{
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glFlush();
}

// ── game_update: submit the full scene through GL APIs ─
extern "C" void game_update(void)
{
    s_angle += 2.0f;
    if (s_angle >= 360.0f) s_angle -= 360.0f;
    /* Skybox rotates slower than the cube. */
    s_sky_angle += 0.8f;
    if (s_sky_angle >= 360.0f) s_sky_angle -= 360.0f;
    /* Slow vertical drift for the reflection overlay. */
    s_scroll_v += 0.001f;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);

    /* Skybox camera (rotation only) — rotates SLOWER than the cube. */
    glLoadIdentity();
    glRotatef(25, 1, 0, 0);
    glRotatef(s_sky_angle, 0, 1, 0);
    draw_skybox();

    /* Scene camera */
    glLoadIdentity();
    glTranslatef(0, 0, -3.5f);
    glRotatef(25, 1, 0, 0);
    glRotatef(s_angle, 0, 1, 0);

    /* Metal cube (the scene's only object) */
    draw_metal_cube(0.8f);

    glFlush();
}

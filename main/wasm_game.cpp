// wasm_game.cpp — Xiaomiao 3D game, submitted entirely through GL APIs.
//
// Runs on core 0 via the WAMR fast interpreter. It is the sole author of every
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
IMPORT(int,  game_get_key);

/* ── New: Lighting / Materials ── */
IMPORT(void, glMaterialfv, int mode, int type, float v0, float v1, float v2, float v3);
IMPORT(void, glMaterialf,  int mode, int type, float v);
IMPORT(void, glLightfv,    int light, int type, float v0, float v1, float v2, float v3);
IMPORT(void, glLightModeli, int pname, int param);
IMPORT(void, glColorMaterial, int mode, int type);

/* ── New: Transform / View ── */
IMPORT(void, glScalef,    float x, float y, float z);
IMPORT(void, glViewport,  int x, int y, int w, int h);
IMPORT(void, glFrustum,   double l, double r, double b, double t, double n, double f);
IMPORT(void, glShadeModel, int mode);

/* ── New: Blend ── */
IMPORT(void, glBlendFunc, int sfactor, int dfactor);

/* ── New: Clear ── */
IMPORT(void, glClearColor, float r, float g, float b, float a);

// ── GL constants ────────────────────────────────────────
enum {
    GL_QUADS             = 0x0007,
    GL_DEPTH_TEST        = 0x0B71,
    GL_CULL_FACE         = 0x0B44,
    GL_LIGHTING          = 0x0B50,
    GL_LIGHT0            = 0x4000,
    GL_MODELVIEW         = 0x1700,
    GL_PROJECTION        = 0x1701,
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

    /* ── Lighting / Materials ── */
    GL_AMBIENT            = 0x1200,
    GL_DIFFUSE            = 0x1201,
    GL_SPECULAR           = 0x1202,
    GL_EMISSION           = 0x1600,
    GL_SHININESS          = 0x1601,
    GL_POSITION           = 0x1203,
    GL_AMBIENT_AND_DIFFUSE = 0x1602,
    GL_LIGHT_MODEL_LOCAL_VIEWER = 0x0B51,
    GL_LIGHT_MODEL_TWO_SIDE     = 0x0B52,
    GL_COLOR_MATERIAL     = 0x0B57,
    GL_FRONT_AND_BACK     = 0x0408,

    /* ── Shading ── */
    GL_FLAT   = 0x1D00,
    GL_SMOOTH = 0x1D01,

    /* ── Blend ── */
    GL_BLEND              = 0x0BE2,
    GL_SRC_ALPHA          = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_ZERO               = 0,
    GL_ONE                = 1,
};

// ── Key event encoding (matches wasm_game.c) ────────────
// game_get_key() returns (btn_idx << 2) | edge, or 0 for empty.
enum {
    KEY_EDGE_DOWN = 0,
    KEY_EDGE_UP   = 1,
    BTN_UP        = 0,
    BTN_DOWN      = 1,
    BTN_LEFT      = 2,
    BTN_RIGHT     = 3,
    BTN_A         = 4,
    BTN_B         = 5,
};

// Texture names (firmware-integrated; integer names, no data in the API)
// IDs 1-10 are uploaded in tinygl_test.c at boot.
enum {
    TEX_CERAMIC  = 1,
    TEX_CHECKER  = 2,
    TEX_BRICK    = 3,
    TEX_GRID     = 4,
    TEX_SKY      = 5,
    TEX_SAND     = 6,
    TEX_HORIZON  = 7,
    TEX_METAL    = 8,
    TEX_SPECULAR = 9,
    TEX_REFLECT  = 10,
};

// ── Material presets ────────────────────────────────────
// Each material is a complete triple: base texture + overlay1 + overlay2 with
// appropriate weights for the material's look.
struct material_t { int base; int ov1; int ov2; float w1; float w2; bool metal; };
static const material_t s_materials[] = {
    // base        overlay1     overlay2     w1     w2   metal?
    { TEX_METAL,   TEX_REFLECT, TEX_SPECULAR, 1.0f,  1.2f, true  },  // 金属
    { TEX_BRICK,   0,           0,            0.0f,  0.0f, false },  // 砖块
    { TEX_SAND,    0,           0,            0.0f,  0.0f, false },  // 沙石
};
static const int MATERIAL_COUNT = sizeof(s_materials) / sizeof(s_materials[0]);
static int s_material_idx = 0;  /* current material index */

// ── Global state ────────────────────────────────────────
static float s_angle = 0.0f;       /* cube rotation (fast) */
static float s_sky_angle = 0.0f;   /* skybox rotation (slower) */
static int s_label_show = 0;       /* frames remaining for texture name label */

// ── Draw a cube of half-size hs, centred at origin ─────
// eye_x/y/z: viewer position in the cube's LOCAL frame (only used for metal
// reflection mapping; other textures use a fixed distant viewer (0,0,5)).
static void cube(float hs, float eye_x, float eye_y, float eye_z)
{
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
            // view ray: from surface point toward the viewer (in local frame)
            float ix = eye_x - vx, iy = eye_y - vy, iz = eye_z - vz;
            float il = __builtin_sqrtf(ix*ix + iy*iy + iz*iz);
            if (il > 0.001f) { ix /= il; iy /= il; iz /= il; }
            // reflect across the face normal
            float nd = F[f].n[0]*ix + F[f].n[1]*iy + F[f].n[2]*iz;
            float rx = ix - 2*nd*F[f].n[0];
            float ry = iy - 2*nd*F[f].n[1];
            // sphere-map: s from Rx, t from Ry
            // (ry>0 = pointing up = should reflect sky = top of texture = t small)
            float s = (rx + 1) * 0.5f;
            float t = (1 - ry) * 0.5f;
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

// ── Draw the textured cube with configurable layers ─────
static void draw_cube(float hs, const material_t *mat)
{
    /* Unit 0: base texture */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mat->base);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    /* Unit 1: overlay 1 (ADD) */
    if (mat->ov1 && mat->w1 > 0.01f) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mat->ov1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, mat->w1, mat->w1, mat->w1, 1.0f);
    } else {
        glActiveTexture(GL_TEXTURE1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    }

    /* Unit 2: overlay 2 (ADD) */
    if (mat->ov2 && mat->w2 > 0.01f) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, mat->ov2);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, mat->w2, mat->w2, mat->w2, 1.0f);
        /* Animated specular sweep */
        if (mat->ov2 == TEX_SPECULAR) {
            float su = s_angle * 0.3f;
            float sv = s_angle * 0.15f;
            glTexOffset(GL_TEXTURE2, su, sv);
        }
    } else {
        glActiveTexture(GL_TEXTURE2);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    }

    // Compute viewer position in the cube's LOCAL frame.
    // Metal materials: full inverse modelview for environment reflection.
    // Non-metal: fixed distant viewer (0,0,5) — no reflection needed.
    {
        float eye_x = 0.0f, eye_y = 0.0f, eye_z = 5.0f;

        if (mat->metal) {
            float a  = s_angle * 3.14159265f / 180.0f;
            float ax = a * 0.6f;
            float ay = a;
            float px = 25.0f * 3.14159265f / 180.0f;

            float ca = __builtin_cosf(ax), sa = __builtin_sinf(ax);
            float cb = __builtin_cosf(ay), sb = __builtin_sinf(ay);
            float cp = __builtin_cosf(px), sp = __builtin_sinf(px);

            float vx = 0.0f, vy = 0.0f, vz = 3.5f;

            // inv(R_x(25))
            { float t = vy; vy = t*cp + vz*sp; vz = -t*sp + vz*cp; }
            // inv(R_y(a)) — scene yaw
            { float t = vx; vx = t*cb - vz*sb; vz = t*sb + vz*cb; }
            // inv(R_y(a)) — cube Y
            { float t = vx; vx = t*cb - vz*sb; vz = t*sb + vz*cb; }
            // inv(R_x(0.6a))
            { float t = vy; vy = t*ca + vz*sa; vz = -t*sa + vz*ca; }

            float len = __builtin_sqrtf(vx*vx + vy*vy + vz*vz);
            if (len > 0.001f) { vx /= len; vy /= len; vz /= len; }
            eye_x = vx * 5.0f;
            eye_y = vy * 5.0f;
            eye_z = vz * 5.0f;
        }

        glActiveTexture(GL_TEXTURE0);
        glPushMatrix();
        glRotatef(s_angle, 0, 1, 0);
        glRotatef(s_angle * 0.6f, 1, 0, 0);
        cube(hs, eye_x, eye_y, eye_z);
        glPopMatrix();
    }

    /* Reset units 1-2 to REPLACE so subsequent draws aren't multi-textured. */
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

    /* Directional light from front-above-right: brightens all visible faces
     * with a warm-white tint. Normalised dir ≈ (0.64, 0.53, 0.55). */
    glLightfv(GL_LIGHT0, GL_POSITION, 3.0f, 2.5f, 2.6f, 0.0f);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  1.0f, 1.0f, 1.0f, 1.0f);
    glLightfv(GL_LIGHT0, GL_SPECULAR, 1.0f, 1.0f, 1.0f, 1.0f);

    /* Default material: bright diffuse, strong specular highlight */
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE,   0.4f, 0.4f, 0.4f, 1.0f);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT,   0.2f, 0.2f, 0.2f, 1.0f);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR,  0.7f, 0.7f, 0.7f, 1.0f);
    glMaterialf (GL_FRONT_AND_BACK, GL_SHININESS, 60.0f);

    /* Enable per-vertex color tracking so cube() can tint faces */
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    /* Set default vertex color to white so material ambient/diffuse tracks it */
    glColor3f(1.0f, 1.0f, 1.0f);

    /* Dark gray clear color (dark sky look) */
    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glFlush();
}

// ── Consume key events ──────────────────────────────────
static void handle_keys(void)
{
    for (;;) {
        int ev = game_get_key();
        if (!ev) break;

        int btn  = ev >> 2;
        int edge = ev & 3;

        if (edge != KEY_EDGE_DOWN) continue;  /* only act on press */

        if (btn == BTN_A) {
            s_material_idx = (s_material_idx + 1) % MATERIAL_COUNT;
            s_label_show = 45;
        }
    }
}

// ── game_update: submit the full scene through GL APIs ─
extern "C" void game_update(void)
{
    s_angle += 2.0f;
    if (s_angle >= 360.0f) s_angle -= 360.0f;
    s_sky_angle += 0.8f;
    if (s_sky_angle >= 360.0f) s_sky_angle -= 360.0f;
    if (s_label_show > 0) s_label_show--;

    handle_keys();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);

    /* Skybox camera */
    glLoadIdentity();
    glRotatef(25, 1, 0, 0);
    glRotatef(s_sky_angle, 0, 1, 0);
    draw_skybox();

    /* Scene camera */
    glLoadIdentity();
    glTranslatef(0, 0, -3.5f);
    glRotatef(25, 1, 0, 0);
    glRotatef(s_angle, 0, 1, 0);

    /* Draw the cube with current material */
    draw_cube(0.8f, &s_materials[s_material_idx]);

    glFlush();
}

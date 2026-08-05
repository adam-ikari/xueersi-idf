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
IMPORT(void, glTexGeni, int coord, int pname, int param);
IMPORT(void, glSetEnableSpecular, int flag);
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

    /* ── Reflection / Sphere-map (standard OpenGL) ── */
    GL_TEXTURE_GEN_S      = 0x0C60,
    GL_TEXTURE_GEN_T      = 0x0C61,
    GL_TEXTURE_GEN_MODE   = 0x2500,
    GL_SPHERE_MAP         = 0x2402,
    GL_S                  = 0x2000,
    GL_T                  = 0x2001,
    GL_NORMALIZE          = 0x0BA1,

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
// Each material pairs a base texture with an optional reflection overlay
// (TEX_REFLECT) applied via standard OpenGL sphere-map generation on unit1:
// WASM enables GL_TEXTURE_GEN_S/T + GL_SPHERE_MAP, and TinyGL computes the
// reflection coords from each vertex's eye-space normal (vertex.c), so the
// reflection walks as the cube rotates. Bumpiness is baked into the base
// texture (TEX_METAL) as color shading — no normal/height maps — so the
// base must use FIXED per-face UV or the bumps walk. Specular quality is
// driven by glMaterialfv(GL_SPECULAR) + glMaterialf(GL_SHININESS) through
// TinyGL's Blinn-Phong path (light.c), NOT by a TEX_SPECULAR overlay.
struct material_t {
    int base;           /* unit0 base texture (fixed UV) */
    int reflect;        /* unit1 reflection overlay (ADD + sphere-map gen), 0=off */
    float refl_w;       /* unit1 ADD weight */
    float spec[4];      /* GL_SPECULAR rgba */
    float shininess;    /* GL_SHININESS */
    bool metal;         /* enable reflection */
};
static const material_t s_materials[] = {
    // base        reflect      refl_w  spec                    shin   metal?
    { TEX_METAL,   TEX_REFLECT, 0.5f,   {0.85f,0.85f,0.85f,1.0f}, 90.0f, true  },  // 金属
    { TEX_BRICK,   0,           0.0f,   {0.10f,0.10f,0.10f,1.0f},  8.0f, false },  // 砖块
    { TEX_SAND,    0,           0.0f,   {0.15f,0.15f,0.15f,1.0f}, 12.0f, false },  // 沙石
};
static const int MATERIAL_COUNT = sizeof(s_materials) / sizeof(s_materials[0]);
static int s_material_idx = 0;  /* current material index */

// ── Global state ────────────────────────────────────────
static float s_angle = 0.0f;       /* cube rotation (fast) */
static float s_sky_angle = 0.0f;   /* skybox rotation (slower) */
static int s_label_show = 0;       /* frames remaining for texture name label */

// ── Draw a cube of half-size hs, centred at origin ─────
// Fixed per-face UV (0..1) so the base texture's baked bumps stay put.
// Per-vertex normals drive both TinyGL's Blinn-Phong specular (via
// glMaterialfv) and the unit1 sphere-map reflection coords (generated by
// TinyGL in vertex.c when GL_TEXTURE_GEN_S/T is enabled in draw_cube).
static void cube(float hs)
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
    /* fixed per-corner UV (matches tinygl_test.c draw_metal_cube) */
    static const float UV[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            glNormal3f(F[f].n[0], F[f].n[1], F[f].n[2]);
            glTexCoord2f(UV[k][0], UV[k][1]);
            glVertex3f(F[f].v[k][0], F[f].v[k][1], F[f].v[k][2]);
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
    /* Per-material specular via OpenGL API (drives TinyGL Blinn-Phong). */
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat->spec[0], mat->spec[1],
                 mat->spec[2], mat->spec[3]);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, mat->shininess);

    /* Unit 0: base texture (fixed UV — baked bumps stay put) */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mat->base);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    /* Unit 1: reflection overlay (ADD + sphere-map gen).
     * Standard OpenGL sphere mapping: TinyGL generates reflection coords from
     * the per-vertex eye-space normal (vertex.c), so the reflection walks as
     * the cube rotates. TEX_METAL's fixed UV on unit0 is unaffected — gen
     * coords are an independent second texture coordinate stream. */
    if (mat->reflect && mat->refl_w > 0.01f) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mat->reflect);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR,
                   mat->refl_w, mat->refl_w, mat->refl_w, 1.0f);
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
    } else {
        glActiveTexture(GL_TEXTURE1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
    }

    /* Unit 2: unused (specular is now API-driven, no TEX_SPECULAR overlay). */
    glActiveTexture(GL_TEXTURE2);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE0);
    glPushMatrix();
    glRotatef(s_angle, 0, 1, 0);
    glRotatef(s_angle * 0.6f, 1, 0, 0);
    cube(hs);
    glPopMatrix();

    /* Reset units 1-2 to REPLACE so subsequent draws aren't multi-textured.
     * Explicitly disable sphere-map gen so the skybox (single-textured) and
     * any later non-reflective geometry never inherit reflection coords. */
    glActiveTexture(GL_TEXTURE1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
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

    /* Enable TinyGL's Blinn-Phong specular path (gated by zEnableSpecular,
     * which is otherwise 0 and never set without this call). Normalise
     * eye-space normals so lighting and sphere-map reflection are correct
     * under non-uniform modelview scaling. */
    glSetEnableSpecular(1);
    glEnable(GL_NORMALIZE);

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

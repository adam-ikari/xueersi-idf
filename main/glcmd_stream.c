/**
 * TinyGL GL command stream — wasm → native bridge.
 *
 * N-buffer zero-copy ping-pong: core 0 (wasm game) encodes GL calls into one
 * of N frame buffers; core 1 drains and replays them against the real TinyGL
 * context. Producer and consumer each own their own index, so no lock is
 * needed and there is no per-frame memcpy. This makes the wasm the sole author
 * of every rendered scene.
 */
#include "glcmd_stream.h"
#include "GL/gl.h"
#include "zgl.h"
#include <string.h>
#include "esp_log.h"

/* ── Encoder (core 0) ───────────────────────────────────── */

/* N buffers; producer (core 0) and consumer (core 1) each own their own
 * index, so no lock is needed. s_len[i]==0 → buffer free for the producer. */
static uint8_t s_buf[GLCMD_NUM_BUFFERS][GLCMD_BUFFER_SIZE];
static volatile uint32_t s_len[GLCMD_NUM_BUFFERS];
static uint32_t s_pos = 0;
static uint32_t s_enc_idx = 0;   /* written only by core 0 */
static uint32_t s_dec_idx = 0;   /* written only by core 1 */

void glcmd_begin_frame(void)
{
    /* Steady state (30fps lockstep): already drained → no spin. */
    while (s_len[s_enc_idx] != 0) { }
    s_pos = 0;
}

void glcmd_publish(void)
{
    __sync_synchronize();              /* release: frame data visible before flag */
    s_len[s_enc_idx] = s_pos;
    s_enc_idx = (s_enc_idx + 1) % GLCMD_NUM_BUFFERS;
}

bool glcmd_u8(uint8_t v) {
    if (s_pos < GLCMD_BUFFER_SIZE) { s_buf[s_enc_idx][s_pos++] = v; return true; }
    return false;
}

bool glcmd_u32(uint32_t v) {
    if (s_pos + 4 <= GLCMD_BUFFER_SIZE) { memcpy(s_buf[s_enc_idx] + s_pos, &v, 4); s_pos += 4; return true; }
    return false;
}

bool glcmd_f32(float v) {
    uint32_t r;
    memcpy(&r, &v, 4);
    return glcmd_u32(r);
}

/* ── Decoder (core 1) ───────────────────────────────────── */

const uint8_t *glcmd_frame_poll(uint32_t *out_len)
{
    uint32_t len = s_len[s_dec_idx];
    if (!len) return NULL;
    *out_len = len;
    return s_buf[s_dec_idx];
}

void glcmd_frame_release(void)
{
    __sync_synchronize();              /* release: replay reads done before clear */
    s_len[s_dec_idx] = 0;
    s_dec_idx = (s_dec_idx + 1) % GLCMD_NUM_BUFFERS;
}

/* Read helpers — advance p, return value */
static inline uint32_t read_u32(const uint8_t **p) {
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

static inline float read_f32(const uint8_t **p) {
    float v;
    uint32_t raw;
    memcpy(&raw, *p, 4);
    memcpy(&v, &raw, 4);
    *p += 4;
    return v;
}

static inline double read_f64(const uint8_t **p) {
    double v;
    uint64_t raw;
    uint32_t lo, hi;
    lo = read_u32(p);
    hi = read_u32(p);
    raw = ((uint64_t)hi << 32) | lo;
    memcpy(&v, &raw, 8);
    return v;
}

/* ── Decoder (core 1) ───────────────────────────────────── */

uint32_t glcmd_replay(const uint8_t *buf, uint32_t len)
{
    const uint8_t *p = buf;
    const uint8_t *end = buf + len;

    while (p < end) {
        uint8_t op = *p++;

        switch (op) {
        case GLCMD_BEGIN:
            glBegin((GLint)read_u32(&p));
            break;
        case GLCMD_END:
            glEnd();
            break;
        case GLCMD_VERTEX3F:
            glVertex3f(read_f32(&p), read_f32(&p), read_f32(&p));
            break;
        case GLCMD_COLOR3F:
            glColor3f(read_f32(&p), read_f32(&p), read_f32(&p));
            break;
        case GLCMD_NORMAL3F:
            glNormal3f(read_f32(&p), read_f32(&p), read_f32(&p));
            break;
        case GLCMD_TEXCOORD2F:
            glTexCoord2f(read_f32(&p), read_f32(&p));
            break;
        case GLCMD_MATRIX_MODE:
            glMatrixMode((GLint)read_u32(&p));
            break;
        case GLCMD_LOAD_IDENTITY:
            glLoadIdentity();
            break;
        case GLCMD_PUSH_MATRIX:
            glPushMatrix();
            break;
        case GLCMD_POP_MATRIX:
            glPopMatrix();
            break;
        case GLCMD_ROTATEF:
            glRotatef(read_f32(&p), read_f32(&p), read_f32(&p), read_f32(&p));
            break;
        case GLCMD_TRANSLATEF:
            glTranslatef(read_f32(&p), read_f32(&p), read_f32(&p));
            break;
        case GLCMD_BIND_TEXTURE: {
            GLint target = (GLint)read_u32(&p);
            GLint tex    = (GLint)read_u32(&p);
            glBindTexture(target, tex);
            break;
        }
        case GLCMD_ACTIVE_TEXTURE: {
            GLenum u = (GLenum)read_u32(&p);
            static int d = 0;
            if (d++ < 10) ESP_LOGI("dbg", "REPLAY activeTex 0x%x", u);
            glActiveTexture(u);
            break;
        }
        case GLCMD_TEX_ENVI: {
            GLint target = (GLint)read_u32(&p);
            GLint pname  = (GLint)read_u32(&p);
            GLint param  = (GLint)read_u32(&p);
            static int d = 0;
            if (d++ < 10) ESP_LOGI("dbg", "REPLAY texEnvi t=0x%x p=0x%x v=0x%x (active unit %d)",
                                    target, pname, param, gl_get_context()->active_texture_unit);
            glTexEnvi(target, pname, param);
            break;
        }
        case GLCMD_TEX_ENVFV: {
            GLenum target = (GLenum)read_u32(&p);
            GLenum pname  = (GLenum)read_u32(&p);
            GLfloat params[4];
            params[0] = read_f32(&p);
            params[1] = read_f32(&p);
            params[2] = read_f32(&p);
            params[3] = read_f32(&p);
            glTexEnvfv(target, pname, params);
            break;
        }
        case GLCMD_TEX_OFFSET: {
            GLenum unit = (GLenum)read_u32(&p);
            float u = read_f32(&p);
            float v = read_f32(&p);
            glTexOffset(unit, u, v);
            break;
        }
        case GLCMD_ENABLE:
            glEnable((GLint)read_u32(&p));
            break;
        case GLCMD_DISABLE:
            glDisable((GLint)read_u32(&p));
            break;
        case GLCMD_DEPTH_MASK:
            glDepthMask((GLint)read_u32(&p));
            break;
        case GLCMD_CLEAR:
            glClear((GLint)read_u32(&p));
            break;

        /* ── Lighting / Materials ── */
        case GLCMD_MATERIAL_FV: {
            GLint mode = (GLint)read_u32(&p);
            GLint type = (GLint)read_u32(&p);
            GLfloat params[4] = { read_f32(&p), read_f32(&p), read_f32(&p), read_f32(&p) };
            glMaterialfv(mode, type, params);
            break;
        }
        case GLCMD_MATERIAL_F: {
            GLint mode = (GLint)read_u32(&p);
            GLint type = (GLint)read_u32(&p);
            GLfloat param = read_f32(&p);
            glMaterialf(mode, type, param);
            break;
        }
        case GLCMD_LIGHT_FV: {
            GLint light = (GLint)read_u32(&p);
            GLint type  = (GLint)read_u32(&p);
            GLfloat params[4] = { read_f32(&p), read_f32(&p), read_f32(&p), read_f32(&p) };
            glLightfv(light, type, params);
            break;
        }
        case GLCMD_LIGHT_MODEL_I: {
            GLint pname = (GLint)read_u32(&p);
            GLint param = (GLint)read_u32(&p);
            glLightModeli(pname, param);
            break;
        }
        case GLCMD_COLOR_MATERIAL: {
            GLint mode = (GLint)read_u32(&p);
            GLint type = (GLint)read_u32(&p);
            glColorMaterial(mode, type);
            break;
        }

        /* ── Transform / View ── */
        case GLCMD_SCALE_F:
            glScalef(read_f32(&p), read_f32(&p), read_f32(&p));
            break;
        case GLCMD_VIEWPORT:
            glViewport((GLint)read_u32(&p), (GLint)read_u32(&p),
                       (GLint)read_u32(&p), (GLint)read_u32(&p));
            break;
        case GLCMD_FRUSTUM: {
            GLdouble l = read_f64(&p), r = read_f64(&p), b = read_f64(&p);
            GLdouble t = read_f64(&p), n = read_f64(&p), f = read_f64(&p);
            glFrustum(l, r, b, t, n, f);
            break;
        }
        case GLCMD_SHADE_MODEL:
            glShadeModel((GLint)read_u32(&p));
            break;

        /* ── Blend ── */
        case GLCMD_BLEND_FUNC:
            glBlendFunc((GLint)read_u32(&p), (GLint)read_u32(&p));
            break;

        /* ── Clear ── */
        case GLCMD_CLEAR_COLOR:
            glClearColor(read_f32(&p), read_f32(&p), read_f32(&p), read_f32(&p));
            break;

        case GLCMD_NOP:
        default:
            break;
        }
    }

    return (uint32_t)(p - buf);
}

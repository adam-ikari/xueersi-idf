/*
 * Template-based triangle rasterizer with compile-time feature selection.
 *
 * This file generates specialized triangle rasterization functions for all
 * combinations of depth_test and depth_write flags, eliminating per-pixel
 * runtime branching in the hot rasterization loops.
 *
 * Each base function type generates 4 variants:
 *   _DT0_DW0: depth_test=off, depth_write=off
 *   _DT0_DW1: depth_test=off, depth_write=on
 *   _DT1_DW0: depth_test=on, depth_write=off
 *   _DT1_DW1: depth_test=on, depth_write=on
 */

#include <stdlib.h>
#include "msghandling.h"
#include "zbuffer.h"
#include "zgl.h" /* GLTextureUnit, MAX_TEXTURE_UNITS for multi-texture */
#include "ztriangle_variants.h"

#if (TGL_FEATURE_RENDER_BITS != 32) && (TGL_FEATURE_RENDER_BITS != 16)
#error "Incorrect render bits"
#endif

/* Polygon stipple support */
#if TGL_HAS(POLYGON_STIPPLE)
#define TGL_STIPPLEVARS                             \
    GLubyte *zbstipplepattern = zb->stipplepattern; \
    GLubyte zbdostipple = zb->dostipple;
#define THE_X ((GLint) (pp - pp1))
#define XSTIP(_a) ((THE_X + _a) & TGL_POLYGON_STIPPLE_MASK_X)
#define YSTIP (the_y & TGL_POLYGON_STIPPLE_MASK_Y)
#define STIPBIT(_a)                                                       \
    (zbstipplepattern                                                     \
         [(XSTIP(_a) | (YSTIP << TGL_POLYGON_STIPPLE_POW2_WIDTH)) >> 3] & \
     (1 << (XSTIP(_a) & 7)))
#define STIPTEST(_a) &&(!(zbdostipple && !STIPBIT(_a)))
#else
#define TGL_STIPPLEVARS
#define STIPTEST(_a)
#endif

/* No-draw color support */
#if TGL_HAS(NO_DRAW_COLOR)
#define NODRAWTEST(c) &&((c & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#else
#define NODRAWTEST(c)
#endif

/* Exact vertex color of an unlit full-bright vertex (glColor 1,1,1 with
 * GL_LIGHTING off). Matches vertex.c gl_transform_to_viewport_vertex_c.
 * For such triangles RGB_MIX_FUNC is mathematically the identity, but its
 * int32 multiply (0xfeffff * 8-bit channel) overflows on bright texels and
 * scrambles colors — so skip it and write the raw texel instead. */
#define TGL_FULL_BRIGHT (((GLint)(1.0f * COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT)) & COLOR_MASK)  /* 0xfeffff */

/* Per-triangle precomputed extra-unit blend params (filled in DRAW_INIT). */
typedef struct {
    const uint8_t *data; /* texture pixmap */
    int s_off, t_off;    /* integer UV offset added per pixel */
    int w8;              /* blend weight in 0..256 */
} tgl_extra_tex_t;

/* GL_ADD multi-texture blend: unit 0 (default_tex) plus each active extra unit
 * (w/8 * tex), clamped. Channels are extracted on the LOGICAL layout
 * (unswapped); result re-swapped so callers see a byte-swapped word. */
static inline PIXEL tgl_multitex_sample_prepared(
    const tgl_extra_tex_t *extra, int n_extra,
    int s, int t, const uint8_t *default_tex)
{
    PIXEL result = default_tex ? *(PIXEL *)(default_tex + ST_TO_TEXTURE_BYTE_OFFSET(s, t)) : 0;
    for (int i = 0; i < n_extra; i++) {
        int s2 = s + extra[i].s_off;
        int t2 = t + extra[i].t_off;
        PIXEL c = *(PIXEL *)(extra[i].data + ST_TO_TEXTURE_BYTE_OFFSET(s2, t2));
        int w8 = extra[i].w8;
        PIXEL u0 = TGL_BSWAP16(result);
        PIXEL uc = TGL_BSWAP16(c);
        int r1 = (u0 >> 11) & 0x1f, g1 = (u0 >> 5) & 0x3f, b1 = u0 & 0x1f;
        int r2 = (uc >> 11) & 0x1f, g2 = (uc >> 5) & 0x3f, b2 = uc & 0x1f;
        int r = r1 + ((r2 * w8) >> 8);
        int g = g1 + ((g2 * w8) >> 8);
        int b = b1 + ((b2 * w8) >> 8);
        if (r > 31) r = 31;
        if (g > 63) g = 63;
        if (b > 31) b = 31;
        result = TGL_BSWAP16((PIXEL)((r << 11) | (g << 5) | b));
    }
    return result;
}

/* Diagnostic: triangles rasterized with multi-texture active (core 1). */
volatile int tgl_multitex_tris = 0;
volatile int tgl_dbg_addmode = 0; /* any unit 1+ with env_mode == GL_ADD */

#define TGL_MULTITEX_SAMPLE(_s, _t)                                          \
    (multitex_active                                                         \
         ? tgl_multitex_sample_prepared(tgl_extra, tgl_n_extra, _s, _t,      \
                                        (const uint8_t *) texture)           \
         : TEXTURE_SAMPLE(texture, _s, _t))

#define TGL_MULTITEX_INIT()                                                   \
    {                                                                         \
        unlit = (p0->r == TGL_FULL_BRIGHT && p0->g == TGL_FULL_BRIGHT &&      \
                 p0->b == TGL_FULL_BRIGHT && p1->r == TGL_FULL_BRIGHT &&      \
                 p1->g == TGL_FULL_BRIGHT && p1->b == TGL_FULL_BRIGHT &&      \
                 p2->r == TGL_FULL_BRIGHT && p2->g == TGL_FULL_BRIGHT &&      \
                 p2->b == TGL_FULL_BRIGHT);                                   \
        tgl_units = &gl_get_context()->tex_unit[0];                           \
        tgl_n_extra = 0;                                                      \
        for (int _i = 1; _i < MAX_TEXTURE_UNITS; _i++) {                      \
            GLTextureUnit *_u = &tgl_units[_i];                               \
            if (_u->env_mode == GL_ADD) tgl_dbg_addmode++;                    \
            if (_u->env_mode != GL_ADD || !_u->texture ||                     \
                !_u->texture->images[0].xsize)                                \
                continue;                                                     \
            float _w = _u->env_color[0];                                      \
            if (_w <= 0.0f) continue;                                         \
            if (_w > 1.0f) _w = 1.0f;                                         \
            tgl_extra[tgl_n_extra].data =                                    \
                (const uint8_t *) _u->texture->images[0].pixmap;              \
            tgl_extra[tgl_n_extra].s_off =                                   \
                (int) (_u->u_off *                                           \
                       (1 << (1 + TGL_FEATURE_TEXTURE_POW2 +                 \
                              ZB_POINT_S_FRAC_BITS)));                        \
            tgl_extra[tgl_n_extra].t_off =                                   \
                (int) (_u->v_off * (1 << ZB_POINT_T_FRAC_BITS));              \
            tgl_extra[tgl_n_extra].w8 = (int) (_w * 256.0f + 0.5f);           \
            if (tgl_extra[tgl_n_extra].w8 > 256)                              \
                tgl_extra[tgl_n_extra].w8 = 256;                              \
            tgl_n_extra++;                                                    \
        }                                                                     \
        multitex_active = (tgl_n_extra > 0);                                  \
        if (multitex_active) tgl_multitex_tris++;                             \
    }

/* Texture setup */
void ZB_setTexture(ZBuffer *zb, PIXEL *texture)
{
    zb->current_texture = texture;
}

/*
 * ============================================================================
 * Flat shaded triangle - with blending
 * ============================================================================
 */

/* Variant DT0_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlat_DT0_DW0(ZBuffer *zb,
                                 ZBufferPoint *p0,
                                 ZBufferPoint *p1,
                                 ZBufferPoint *p2)
{
    GLuint color;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT()                                \
    {                                              \
        color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        /* DT=0: always pass depth test */              \
        if (1 STIPTEST(_a)) {                           \
            TGL_BLEND_FUNC(color, (pp[_a]))             \
            /* DW=0: no depth write */                  \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/* Variant DT0_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlat_DT0_DW1(ZBuffer *zb,
                                 ZBufferPoint *p0,
                                 ZBufferPoint *p1,
                                 ZBufferPoint *p2)
{
    GLuint color;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT()                                \
    {                                              \
        color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        /* DT=0: always pass depth test */              \
        if (1 STIPTEST(_a)) {                           \
            TGL_BLEND_FUNC(color, (pp[_a]))             \
            /* DW=1: always write depth */              \
            pz[_a] = zz;                                \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlat_DT1_DW0(ZBuffer *zb,
                                 ZBufferPoint *p0,
                                 ZBufferPoint *p1,
                                 ZBufferPoint *p2)
{
    GLuint color;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT()                                \
    {                                              \
        color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        /* DT=1: test depth */                          \
        if ((zz >= pz[_a]) STIPTEST(_a)) {              \
            TGL_BLEND_FUNC(color, (pp[_a]))             \
            /* DW=0: no depth write */                  \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlat_DT1_DW1(ZBuffer *zb,
                                 ZBufferPoint *p0,
                                 ZBufferPoint *p1,
                                 ZBufferPoint *p2)
{
    GLuint color;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT()                                \
    {                                              \
        color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        /* DT=1: test depth */                          \
        if ((zz >= pz[_a]) STIPTEST(_a)) {              \
            TGL_BLEND_FUNC(color, (pp[_a]))             \
            /* DW=1: always write depth */              \
            pz[_a] = zz;                                \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/*
 * ============================================================================
 * Flat shaded triangle - no blending
 * ============================================================================
 */

/* Variant DT0_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlatNOBLEND_DT0_DW0(ZBuffer *zb,
                                        ZBufferPoint *p0,
                                        ZBufferPoint *p1,
                                        ZBufferPoint *p2)
{
    PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if (1 STIPTEST(_a)) {                           \
            pp[_a] = color;                             \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/* Variant DT0_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlatNOBLEND_DT0_DW1(ZBuffer *zb,
                                        ZBufferPoint *p0,
                                        ZBufferPoint *p1,
                                        ZBufferPoint *p2)
{
    PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if (1 STIPTEST(_a)) {                           \
            pp[_a] = color;                             \
            pz[_a] = zz;                                \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlatNOBLEND_DT1_DW0(ZBuffer *zb,
                                        ZBufferPoint *p0,
                                        ZBufferPoint *p1,
                                        ZBufferPoint *p2)
{
    PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if ((zz >= pz[_a]) STIPTEST(_a)) {              \
            pp[_a] = color;                             \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleFlatNOBLEND_DT1_DW1(ZBuffer *zb,
                                        ZBufferPoint *p0,
                                        ZBufferPoint *p1,
                                        ZBufferPoint *p2)
{
    PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
    TGL_STIPPLEVARS

#define INTERP_Z

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if ((zz >= pz[_a]) STIPTEST(_a)) {              \
            pp[_a] = color;                             \
            pz[_a] = zz;                                \
        }                                               \
        z += dzdx;                                      \
    }

#include "ztriangle.h"
}

/*
 * ============================================================================
 * Smooth shaded triangle - with blending
 * ============================================================================
 */

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

/* Variant DT0_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmooth_DT0_DW0(ZBuffer *zb,
                                   ZBufferPoint *p0,
                                   ZBufferPoint *p1,
                                   ZBufferPoint *p2)
{
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                    \
    {                                                    \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;  \
        if (1 STIPTEST(_a)) {                            \
            TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a])); \
        }                                                \
        z += dzdx;                                       \
        og1 += dgdx;                                     \
        or1 += drdx;                                     \
        ob1 += dbdx;                                     \
    }

#include "ztriangle.h"
}

/* Variant DT0_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmooth_DT0_DW1(ZBuffer *zb,
                                   ZBufferPoint *p0,
                                   ZBufferPoint *p1,
                                   ZBufferPoint *p2)
{
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                    \
    {                                                    \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;  \
        if (1 STIPTEST(_a)) {                            \
            TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a])); \
            pz[_a] = zz;                                 \
        }                                                \
        z += dzdx;                                       \
        og1 += dgdx;                                     \
        or1 += drdx;                                     \
        ob1 += dbdx;                                     \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmooth_DT1_DW0(ZBuffer *zb,
                                   ZBufferPoint *p0,
                                   ZBufferPoint *p1,
                                   ZBufferPoint *p2)
{
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                    \
    {                                                    \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;  \
        if ((zz >= pz[_a]) STIPTEST(_a)) {               \
            TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a])); \
        }                                                \
        z += dzdx;                                       \
        og1 += dgdx;                                     \
        or1 += drdx;                                     \
        ob1 += dbdx;                                     \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmooth_DT1_DW1(ZBuffer *zb,
                                   ZBufferPoint *p0,
                                   ZBufferPoint *p1,
                                   ZBufferPoint *p2)
{
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                    \
    {                                                    \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;  \
        if ((zz >= pz[_a]) STIPTEST(_a)) {               \
            TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a])); \
            pz[_a] = zz;                                 \
        }                                                \
        z += dzdx;                                       \
        og1 += dgdx;                                     \
        or1 += drdx;                                     \
        ob1 += dbdx;                                     \
    }

#include "ztriangle.h"
}

/*
 * ============================================================================
 * Smooth shaded triangle - no blending
 * ============================================================================
 */

/* Variant DT0_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmoothNOBLEND_DT0_DW0(ZBuffer *zb,
                                          ZBufferPoint *p0,
                                          ZBufferPoint *p1,
                                          ZBufferPoint *p2)
{
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if (1 STIPTEST(_a)) {                           \
            pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);       \
        }                                               \
        z += dzdx;                                      \
        og1 += dgdx;                                    \
        or1 += drdx;                                    \
        ob1 += dbdx;                                    \
    }

#include "ztriangle.h"
}

/* Variant DT0_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmoothNOBLEND_DT0_DW1(ZBuffer *zb,
                                          ZBufferPoint *p0,
                                          ZBufferPoint *p1,
                                          ZBufferPoint *p2)
{
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if (1 STIPTEST(_a)) {                           \
            pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);       \
            pz[_a] = zz;                                \
        }                                               \
        z += dzdx;                                      \
        og1 += dgdx;                                    \
        or1 += drdx;                                    \
        ob1 += dbdx;                                    \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmoothNOBLEND_DT1_DW0(ZBuffer *zb,
                                          ZBufferPoint *p0,
                                          ZBufferPoint *p1,
                                          ZBufferPoint *p2)
{
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if ((zz >= pz[_a]) STIPTEST(_a)) {              \
            pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);       \
        }                                               \
        z += dzdx;                                      \
        og1 += dgdx;                                    \
        or1 += drdx;                                    \
        ob1 += dbdx;                                    \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE

void ZB_fillTriangleSmoothNOBLEND_DT1_DW1(ZBuffer *zb,
                                          ZBufferPoint *p0,
                                          ZBufferPoint *p1,
                                          ZBufferPoint *p2)
{
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define DRAW_INIT() \
    {               \
    }

#define PUT_PIXEL(_a)                                   \
    {                                                   \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS; \
        if ((zz >= pz[_a]) STIPTEST(_a)) {              \
            pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);       \
            pz[_a] = zz;                                \
        }                                               \
        z += dzdx;                                      \
        og1 += dgdx;                                    \
        or1 += drdx;                                    \
        ob1 += dbdx;                                    \
    }

#include "ztriangle.h"
}

/*
 * ============================================================================
 * Texture mapped triangle - common macros
 * ============================================================================
 */

#if TGL_HAS(LIT_TEXTURES)
#define OR1OG1OB1DECL             \
    register GLint or1, og1, ob1; \
    or1 = r1;                     \
    og1 = g1;                     \
    ob1 = b1;
#define OR1G1B1INCR \
    og1 += dgdx;    \
    or1 += drdx;    \
    ob1 += dbdx;
#else
#define OR1OG1OB1DECL
#define OR1G1B1INCR
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif

#define NB_INTERP 8

#define DRAW_LINE_TRI_TEXTURED(DEPTH_TEST, DEPTH_WRITE_OP)        \
    {                                                             \
        register GLushort *pz;                                    \
        register PIXEL *pp;                                       \
        register GLuint s, t, z;                                  \
        register GLint n;                                         \
        OR1OG1OB1DECL                                             \
        GLfloat sz, tz, fzl, zinv;                                \
        n = (x2 >> 16) - x1;                                      \
        fzl = (GLfloat) z1;                                       \
        zinv = (GLfloat) (1.0 / fzl);                             \
        pp = (PIXEL *) ((GLbyte *) pp1 + x1 * PSZB);              \
        pz = pz1 + x1;                                            \
        z = z1;                                                   \
        sz = sz1;                                                 \
        tz = tz1;                                                 \
        while (n >= (NB_INTERP - 1)) {                            \
            register GLint dsdx, dtdx;                            \
            {                                                     \
                GLfloat ss, tt;                                   \
                ss = (sz * zinv);                                 \
                tt = (tz * zinv);                                 \
                s = (GLint) ss;                                   \
                t = (GLint) tt;                                   \
                dsdx = (GLint) ((dszdx - ss * fdzdx) * zinv);     \
                dtdx = (GLint) ((dtzdx - tt * fdzdx) * zinv);     \
            }                                                     \
            fzl += fndzdx;                                        \
            /* Newton-Raphson iteration for 1/fzl */              \
            zinv = zinv * (2.0f - fzl * zinv);                    \
            zinv = zinv * (2.0f - fzl * zinv);                    \
            PUT_PIXEL_TEXTURED(0, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(1, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(2, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(3, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(4, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(5, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(6, DEPTH_TEST, DEPTH_WRITE_OP)     \
            PUT_PIXEL_TEXTURED(7, DEPTH_TEST, DEPTH_WRITE_OP)     \
            pz += NB_INTERP;                                      \
            pp += NB_INTERP;                                      \
            n -= NB_INTERP;                                       \
            sz += ndszdx;                                         \
            tz += ndtzdx;                                         \
        }                                                         \
        {                                                         \
            register GLint dsdx, dtdx;                            \
            {                                                     \
                GLfloat ss, tt;                                   \
                ss = (sz * zinv);                                 \
                tt = (tz * zinv);                                 \
                s = (GLint) ss;                                   \
                t = (GLint) tt;                                   \
                dsdx = (GLint) ((dszdx - ss * fdzdx) * zinv);     \
                dtdx = (GLint) ((dtzdx - tt * fdzdx) * zinv);     \
            }                                                     \
            while (n >= 0) {                                      \
                PUT_PIXEL_TEXTURED(0, DEPTH_TEST, DEPTH_WRITE_OP) \
                pz += 1;                                          \
                pp++;                                             \
                n -= 1;                                           \
            }                                                     \
        }                                                         \
    }

/*
 * ============================================================================
 * Texture mapped triangle - with blending
 * ============================================================================
 */

/* Variant DT0_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspective_DT0_DW0(ZBuffer *zb,
                                               ZBufferPoint *p0,
                                               ZBufferPoint *p1,
                                               ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                      \
    {                                                                         \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                       \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                  \
        if (1 STIPTEST(_a) NODRAWTEST(c)) {                                   \
            TGL_BLEND_FUNC(unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c),        \
                           (pp[_a]));                                         \
        }                                                                     \
        z += dzdx;                                                            \
        s += dsdx;                                                            \
        t += dtdx;                                                            \
        OR1G1B1INCR                                                           \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(0, /* no-op */;) \
    }

#include "ztriangle.h"
}

/* Variant DT0_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspective_DT0_DW1(ZBuffer *zb,
                                               ZBufferPoint *p0,
                                               ZBufferPoint *p1,
                                               ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                      \
    {                                                                         \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                       \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                  \
        if (1 STIPTEST(_a) NODRAWTEST(c)) {                                   \
            TGL_BLEND_FUNC(unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c),        \
                           (pp[_a]));                                         \
            pz[_a] = zz;                                                      \
        }                                                                     \
        z += dzdx;                                                            \
        s += dsdx;                                                            \
        t += dtdx;                                                            \
        OR1G1B1INCR                                                           \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(0, pz[_a] = zz;) \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspective_DT1_DW0(ZBuffer *zb,
                                               ZBufferPoint *p0,
                                               ZBufferPoint *p1,
                                               ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                      \
    {                                                                         \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                       \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                  \
        if ((zz >= pz[_a]) STIPTEST(_a) NODRAWTEST(c)) {                      \
            TGL_BLEND_FUNC(unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c),        \
                           (pp[_a]));                                         \
        }                                                                     \
        z += dzdx;                                                            \
        s += dsdx;                                                            \
        t += dtdx;                                                            \
        OR1G1B1INCR                                                           \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(1, /* no-op */;) \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspective_DT1_DW1(ZBuffer *zb,
                                               ZBufferPoint *p0,
                                               ZBufferPoint *p1,
                                               ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_BLEND_VARS
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                      \
    {                                                                         \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                       \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                  \
        if ((zz >= pz[_a]) STIPTEST(_a) NODRAWTEST(c)) {                      \
            TGL_BLEND_FUNC(unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c),        \
                           (pp[_a]));                                         \
            pz[_a] = zz;                                                      \
        }                                                                     \
        z += dzdx;                                                            \
        s += dsdx;                                                            \
        t += dtdx;                                                            \
        OR1G1B1INCR                                                           \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(1, pz[_a] = zz;) \
    }

#include "ztriangle.h"
}

/*
 * ============================================================================
 * Texture mapped triangle - no blending
 * ============================================================================
 */

/* Variant DT0_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspectiveNOBLEND_DT0_DW0(ZBuffer *zb,
                                                      ZBufferPoint *p0,
                                                      ZBufferPoint *p1,
                                                      ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                    \
    {                                                                       \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                     \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                \
        if (1 STIPTEST(_a) NODRAWTEST(c)) {                                 \
            pp[_a] = unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c);            \
        }                                                                   \
        z += dzdx;                                                          \
        s += dsdx;                                                          \
        t += dtdx;                                                          \
        OR1G1B1INCR                                                         \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(0, /* no-op */;) \
    }

#include "ztriangle.h"
}

/* Variant DT0_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspectiveNOBLEND_DT0_DW1(ZBuffer *zb,
                                                      ZBufferPoint *p0,
                                                      ZBufferPoint *p1,
                                                      ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                    \
    {                                                                       \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                     \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                \
        if (1 STIPTEST(_a) NODRAWTEST(c)) {                                 \
            pp[_a] = unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c);            \
            pz[_a] = zz;                                                    \
        }                                                                   \
        z += dzdx;                                                          \
        s += dsdx;                                                          \
        t += dtdx;                                                          \
        OR1G1B1INCR                                                         \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(0, pz[_a] = zz;) \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW0 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspectiveNOBLEND_DT1_DW0(ZBuffer *zb,
                                                      ZBufferPoint *p0,
                                                      ZBufferPoint *p1,
                                                      ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                    \
    {                                                                       \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                     \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                \
        if ((zz >= pz[_a]) STIPTEST(_a) NODRAWTEST(c)) {                    \
            pp[_a] = unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c);            \
        }                                                                   \
        z += dzdx;                                                          \
        s += dsdx;                                                          \
        t += dtdx;                                                          \
        OR1G1B1INCR                                                         \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(1, /* no-op */;) \
    }

#include "ztriangle.h"
}

/* Variant DT1_DW1 */
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#undef DRAW_INIT
#undef PUT_PIXEL
#undef DRAW_LINE
#undef PUT_PIXEL_TEXTURED

void ZB_fillTriangleMappingPerspectiveNOBLEND_DT1_DW1(ZBuffer *zb,
                                                      ZBufferPoint *p0,
                                                      ZBufferPoint *p1,
                                                      ZBufferPoint *p2)
{
    PIXEL *texture;
    int unlit;                /* all 3 vertices full-bright -> RGB_MIX is identity (and would overflow) */
    GLTextureUnit *tgl_units; /* hoisted context tex_unit array */
    int multitex_active;      /* any unit 1+ is GL_ADD + texture + weight */
    tgl_extra_tex_t tgl_extra[3]; /* precomputed extra-unit blend params */
    int tgl_n_extra;
    TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define DRAW_INIT()                    \
    {                                  \
        texture = zb->current_texture; \
        fdzdx = (GLfloat) dzdx;        \
        fndzdx = NB_INTERP * fdzdx;    \
        ndszdx = NB_INTERP * dszdx;    \
        ndtzdx = NB_INTERP * dtzdx;    \
        TGL_MULTITEX_INIT();           \
    }

#define PUT_PIXEL_TEXTURED(_a, _dt, _dw)                                    \
    {                                                                       \
        register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                     \
        PIXEL c = TGL_MULTITEX_SAMPLE(s, t);                                \
        if ((zz >= pz[_a]) STIPTEST(_a) NODRAWTEST(c)) {                    \
            pp[_a] = unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c);            \
            pz[_a] = zz;                                                    \
        }                                                                   \
        z += dzdx;                                                          \
        s += dsdx;                                                          \
        t += dtdx;                                                          \
        OR1G1B1INCR                                                         \
    }

#define DRAW_LINE()                             \
    {                                           \
        DRAW_LINE_TRI_TEXTURED(1, pz[_a] = zz;) \
    }

#include "ztriangle.h"
}

/*
 * ============================================================================
 * Legacy compatibility functions
 * ============================================================================
 *
 * These functions provide backward compatibility with existing code that
 * calls the original function names. They dispatch to the appropriate
 * specialized variant based on runtime state.
 */

/* Dirty rectangle helper macros */
#if TGL_HAS(DIRTY_RECTANGLE)
#define TGL_MIN3(a, b, c) \
    (((a) < (b)) ? (((a) < (c)) ? (a) : (c)) : (((b) < (c)) ? (b) : (c)))
#define TGL_MAX3(a, b, c) \
    (((a) > (b)) ? (((a) > (c)) ? (a) : (c)) : (((b) > (c)) ? (b) : (c)))

#define MARK_TRIANGLE_DIRTY(zb, p0, p1, p2)               \
    do {                                                  \
        GLint xmin = TGL_MIN3((p0)->x, (p1)->x, (p2)->x); \
        GLint xmax = TGL_MAX3((p0)->x, (p1)->x, (p2)->x); \
        GLint ymin = TGL_MIN3((p0)->y, (p1)->y, (p2)->y); \
        GLint ymax = TGL_MAX3((p0)->y, (p1)->y, (p2)->y); \
        ZB_markDirty((zb), xmin, ymin, xmax, ymax);       \
    } while (0)
#endif

void ZB_fillTriangleFlat(ZBuffer *zb,
                         ZBufferPoint *p0,
                         ZBufferPoint *p1,
                         ZBufferPoint *p2)
{
    GLint dt = zb->depth_test != 0;
    GLint dw = zb->depth_write != 0;
    int idx = (dt << 1) | dw;

#if TGL_HAS(DIRTY_RECTANGLE)
    MARK_TRIANGLE_DIRTY(zb, p0, p1, p2);
#endif

    switch (idx) {
    case 0:
        ZB_fillTriangleFlat_DT0_DW0(zb, p0, p1, p2);
        break;
    case 1:
        ZB_fillTriangleFlat_DT0_DW1(zb, p0, p1, p2);
        break;
    case 2:
        ZB_fillTriangleFlat_DT1_DW0(zb, p0, p1, p2);
        break;
    case 3:
        ZB_fillTriangleFlat_DT1_DW1(zb, p0, p1, p2);
        break;
    }
}

void ZB_fillTriangleFlatNOBLEND(ZBuffer *zb,
                                ZBufferPoint *p0,
                                ZBufferPoint *p1,
                                ZBufferPoint *p2)
{
    GLint dt = zb->depth_test != 0;
    GLint dw = zb->depth_write != 0;
    int idx = (dt << 1) | dw;

#if TGL_HAS(DIRTY_RECTANGLE)
    MARK_TRIANGLE_DIRTY(zb, p0, p1, p2);
#endif

    switch (idx) {
    case 0:
        ZB_fillTriangleFlatNOBLEND_DT0_DW0(zb, p0, p1, p2);
        break;
    case 1:
        ZB_fillTriangleFlatNOBLEND_DT0_DW1(zb, p0, p1, p2);
        break;
    case 2:
        ZB_fillTriangleFlatNOBLEND_DT1_DW0(zb, p0, p1, p2);
        break;
    case 3:
        ZB_fillTriangleFlatNOBLEND_DT1_DW1(zb, p0, p1, p2);
        break;
    }
}

void ZB_fillTriangleSmooth(ZBuffer *zb,
                           ZBufferPoint *p0,
                           ZBufferPoint *p1,
                           ZBufferPoint *p2)
{
    GLint dt = zb->depth_test != 0;
    GLint dw = zb->depth_write != 0;
    int idx = (dt << 1) | dw;

#if TGL_HAS(DIRTY_RECTANGLE)
    MARK_TRIANGLE_DIRTY(zb, p0, p1, p2);
#endif

    switch (idx) {
    case 0:
        ZB_fillTriangleSmooth_DT0_DW0(zb, p0, p1, p2);
        break;
    case 1:
        ZB_fillTriangleSmooth_DT0_DW1(zb, p0, p1, p2);
        break;
    case 2:
        ZB_fillTriangleSmooth_DT1_DW0(zb, p0, p1, p2);
        break;
    case 3:
        ZB_fillTriangleSmooth_DT1_DW1(zb, p0, p1, p2);
        break;
    }
}

void ZB_fillTriangleSmoothNOBLEND(ZBuffer *zb,
                                  ZBufferPoint *p0,
                                  ZBufferPoint *p1,
                                  ZBufferPoint *p2)
{
    GLint dt = zb->depth_test != 0;
    GLint dw = zb->depth_write != 0;
    int idx = (dt << 1) | dw;

#if TGL_HAS(DIRTY_RECTANGLE)
    MARK_TRIANGLE_DIRTY(zb, p0, p1, p2);
#endif

    switch (idx) {
    case 0:
        ZB_fillTriangleSmoothNOBLEND_DT0_DW0(zb, p0, p1, p2);
        break;
    case 1:
        ZB_fillTriangleSmoothNOBLEND_DT0_DW1(zb, p0, p1, p2);
        break;
    case 2:
        ZB_fillTriangleSmoothNOBLEND_DT1_DW0(zb, p0, p1, p2);
        break;
    case 3:
        ZB_fillTriangleSmoothNOBLEND_DT1_DW1(zb, p0, p1, p2);
        break;
    }
}

void ZB_fillTriangleMappingPerspective(ZBuffer *zb,
                                       ZBufferPoint *p0,
                                       ZBufferPoint *p1,
                                       ZBufferPoint *p2)
{
    GLint dt = zb->depth_test != 0;
    GLint dw = zb->depth_write != 0;
    int idx = (dt << 1) | dw;

#if TGL_HAS(DIRTY_RECTANGLE)
    MARK_TRIANGLE_DIRTY(zb, p0, p1, p2);
#endif

    switch (idx) {
    case 0:
        ZB_fillTriangleMappingPerspective_DT0_DW0(zb, p0, p1, p2);
        break;
    case 1:
        ZB_fillTriangleMappingPerspective_DT0_DW1(zb, p0, p1, p2);
        break;
    case 2:
        ZB_fillTriangleMappingPerspective_DT1_DW0(zb, p0, p1, p2);
        break;
    case 3:
        ZB_fillTriangleMappingPerspective_DT1_DW1(zb, p0, p1, p2);
        break;
    }
}

void ZB_fillTriangleMappingPerspectiveNOBLEND(ZBuffer *zb,
                                              ZBufferPoint *p0,
                                              ZBufferPoint *p1,
                                              ZBufferPoint *p2)
{
    GLint dt = zb->depth_test != 0;
    GLint dw = zb->depth_write != 0;
    int idx = (dt << 1) | dw;

#if TGL_HAS(DIRTY_RECTANGLE)
    MARK_TRIANGLE_DIRTY(zb, p0, p1, p2);
#endif

    switch (idx) {
    case 0:
        ZB_fillTriangleMappingPerspectiveNOBLEND_DT0_DW0(zb, p0, p1, p2);
        break;
    case 1:
        ZB_fillTriangleMappingPerspectiveNOBLEND_DT0_DW1(zb, p0, p1, p2);
        break;
    case 2:
        ZB_fillTriangleMappingPerspectiveNOBLEND_DT1_DW0(zb, p0, p1, p2);
        break;
    case 3:
        ZB_fillTriangleMappingPerspectiveNOBLEND_DT1_DW1(zb, p0, p1, p2);
        break;
    }
}

/*
 * ============================================================================
 * Dispatch table initialization
 * ============================================================================
 */

ZB_TriangleDispatch zb_triangle_dispatch = {
    /* flat with blend */
    .flat = {ZB_fillTriangleFlat_DT0_DW0, ZB_fillTriangleFlat_DT0_DW1,
             ZB_fillTriangleFlat_DT1_DW0, ZB_fillTriangleFlat_DT1_DW1},
    /* flat without blend */
    .flat_noblend = {ZB_fillTriangleFlatNOBLEND_DT0_DW0,
                     ZB_fillTriangleFlatNOBLEND_DT0_DW1,
                     ZB_fillTriangleFlatNOBLEND_DT1_DW0,
                     ZB_fillTriangleFlatNOBLEND_DT1_DW1},
    /* smooth with blend */
    .smooth = {ZB_fillTriangleSmooth_DT0_DW0, ZB_fillTriangleSmooth_DT0_DW1,
               ZB_fillTriangleSmooth_DT1_DW0, ZB_fillTriangleSmooth_DT1_DW1},
    /* smooth without blend */
    .smooth_noblend = {ZB_fillTriangleSmoothNOBLEND_DT0_DW0,
                       ZB_fillTriangleSmoothNOBLEND_DT0_DW1,
                       ZB_fillTriangleSmoothNOBLEND_DT1_DW0,
                       ZB_fillTriangleSmoothNOBLEND_DT1_DW1},
    /* textured with blend */
    .textured = {ZB_fillTriangleMappingPerspective_DT0_DW0,
                 ZB_fillTriangleMappingPerspective_DT0_DW1,
                 ZB_fillTriangleMappingPerspective_DT1_DW0,
                 ZB_fillTriangleMappingPerspective_DT1_DW1},
    /* textured without blend */
    .textured_noblend = {ZB_fillTriangleMappingPerspectiveNOBLEND_DT0_DW0,
                         ZB_fillTriangleMappingPerspectiveNOBLEND_DT0_DW1,
                         ZB_fillTriangleMappingPerspectiveNOBLEND_DT1_DW0,
                         ZB_fillTriangleMappingPerspectiveNOBLEND_DT1_DW1}};

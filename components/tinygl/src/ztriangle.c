#include "../include/zbuffer.h"
#include "msghandling.h"
#include "zgl.h"   /* GLTextureUnit, MAX_TEXTURE_UNITS for multi-texture */
#include <stdlib.h>

/* Exact vertex color of an unlit full-bright vertex (glColor 1,1,1 with
 * GL_LIGHTING off). Matches vertex.c gl_transform_to_viewport_vertex_c.
 * For such triangles RGB_MIX_FUNC is mathematically the identity, but its
 * int32 multiply (0xfeffff * 8-bit channel) overflows on bright texels and
 * scrambles colors — so skip it and write the raw texel instead. */
#define TGL_FULL_BRIGHT (((GLint)(1.0f * COLOR_CORRECTED_MULT_MASK + COLOR_MIN_MULT)) & COLOR_MASK)  /* 0xfeffff */

/* Per-triangle precomputed extra-unit blend params (filled in DRAW_INIT). */
typedef struct {
    const uint8_t *data;   /* texture pixmap */
    int s_off, t_off;      /* integer UV offset added per pixel (use_gen==0) */
    int w8;                /* blend weight in 0..256 */
    int use_gen;           /* 1 = sample at interpolated sphere-map (s2,t2), 0 = (s+s_off,t+t_off) */
} tgl_extra_tex_t;

/* GL_ADD multi-texture blend: unit 0 (default_tex) plus each active extra unit
 * (w/8 * tex), clamped. Channels extracted on the LOGICAL layout (unswapped);
 * result re-swapped so callers see a byte-swapped word.
 * s2/t2 are the interpolated sphere-map reflection coords (used when the
 * corresponding extra unit has use_gen==1); s/t are the primary coords. */
static inline PIXEL tgl_multitex_sample_prepared(
    const tgl_extra_tex_t *extra, int n_extra,
    int s, int t, int s2, int t2, const uint8_t *default_tex)
{
    PIXEL result = default_tex ? *(PIXEL *)(default_tex + ST_TO_TEXTURE_BYTE_OFFSET(s, t)) : 0;
    for (int i = 0; i < n_extra; i++) {
        int us, ut;
        if (extra[i].use_gen) { us = s2; ut = t2; }
        else { us = s + extra[i].s_off; ut = t + extra[i].t_off; }
        PIXEL c = *(PIXEL *)(extra[i].data + ST_TO_TEXTURE_BYTE_OFFSET(us, ut));
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

/* Diagnostic: count of triangles rasterized with multi-texture active (core 1). */
volatile int tgl_multitex_tris = 0;
volatile int tgl_dbg_addmode = 0;   /* legacy diagnostic counter (no longer incremented in rasterizer) */





/* TODO: Switch from scanline rasterizer to easily parallelized cross product rasterizer.*/
static GLfloat edgeFunction(GLfloat ax, GLfloat ay, GLfloat bx, GLfloat by, GLfloat cx, GLfloat cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

#if TGL_FEATURE_RENDER_BITS == 32
#elif TGL_FEATURE_RENDER_BITS == 16
#else
#error "WRONG MODE!!!"
#endif

#if TGL_FEATURE_POLYGON_STIPPLE == 1

#define TGL_STIPPLEVARS                                                                                                                                        \
	GLubyte* zbstipplepattern = zb->stipplepattern;                                                                                                            \
	GLubyte zbdostipple = zb->dostipple;
#define THE_X ((GLint)(pp - pp1))
#define XSTIP(_a) ((THE_X + _a) & TGL_POLYGON_STIPPLE_MASK_X)
#define YSTIP (the_y & TGL_POLYGON_STIPPLE_MASK_Y)
/* NOTES                                                           Divide by 8 to get the byte        Get the actual bit*/
#define STIPBIT(_a) (zbstipplepattern[(XSTIP(_a) | (YSTIP << TGL_POLYGON_STIPPLE_POW2_WIDTH)) >> 3] & (1 << (XSTIP(_a) & 7)))
#define STIPTEST(_a) &&(!(zbdostipple && !STIPBIT(_a)))

#else

#define TGL_STIPPLEVARS /* a comment */
#define STIPTEST(_a)	/* a comment*/

#endif

#if TGL_FEATURE_NO_DRAW_COLOR == 1
#define NODRAWTEST(c) &&((c & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#else
#define NODRAWTEST(c) /* a comment */
#endif

#define ZCMP(z, zpix, _a, c) (((!zbdt) || (z >= zpix)) STIPTEST(_a) NODRAWTEST(c))
#define ZCMPSIMP(z, zpix, _a, crabapple) (((!zbdt) || (z >= zpix)) STIPTEST(_a))

void ZB_fillTriangleFlat(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdt = zb->depth_test;
	GLubyte zbdw = zb->depth_write;
	GLuint color;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ

#define INTERP_Z


#define DRAW_INIT()                                                                                                                                            \
	{ color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); }

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, color)) {                                                                                                             \
				TGL_BLEND_FUNC(color, (pp[_a])) /*pp[_a] = color;*/                                                                                            \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

void ZB_fillTriangleFlatNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#define INTERP_Z

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = color;                                                                                                                                \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

/*
 * Smooth filled triangle.
 * The code below is very tricky :)
 */

void ZB_fillTriangleSmooth(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}


#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
                                                                                                                                                               \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif

#include "ztriangle.h"
} 

void ZB_fillTriangleSmoothNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}

#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			/*c = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                               \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#endif

#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
                                                                                                                                                               \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif
/* End of 16 bit mode stuff*/
#include "ztriangle.h"
} 

/*


			TEXTURE MAPPED TRIANGLES
               Section_Header




*/
void ZB_setTexture(ZBuffer* zb, PIXEL* texture) { zb->current_texture = texture; }


#if 1

/*
 * ZINV -- reciprocal of z for perspective-correct texture mapping.
 * On ESP32 (single-precision FPU), float division ~20 cycles.
 * The LUT path trades LUT access + interpolation for that division.
 * Toggle via TGL_FEATURE_ZINV_LUT in zfeatures.h, test with `fps` command.
 */
#if TGL_FEATURE_ZINV_LUT == 1
#define ZINV_LUT_SIZE 1024
static uint16_t zinv_lut[ZINV_LUT_SIZE];
static int zinv_lut_inited = 0;

void zinv_lut_init(void) {
    if (zinv_lut_inited) return;
    for (int i = 0; i < ZINV_LUT_SIZE; i++) {
        float z = ((float)i / (float)(ZINV_LUT_SIZE - 1)) * 50.0f + 0.5f;
        float inv = 1.0f / z;
        zinv_lut[i] = (uint16_t)(inv * 65536.0f);
    }
    zinv_lut_inited = 1;
}

static inline float fast_inv_z(float fzl) {
    float fidx = (fzl - 0.5f) / 50.0f * (float)(ZINV_LUT_SIZE - 1);
    if (fidx < 0.0f) fidx = 0.0f;
    if (fidx > (float)(ZINV_LUT_SIZE - 2)) fidx = (float)(ZINV_LUT_SIZE - 2);
    int idx = (int)fidx;
    float frac = fidx - (float)idx;
    uint16_t v0 = zinv_lut[idx];
    uint16_t v1 = zinv_lut[idx + 1];
    float interp = (float)v0 + ((float)v1 - (float)v0) * frac;
    return interp / 65536.0f;
}
#define ZINV(fzl) fast_inv_z(fzl)
#else
#define ZINV(fzl) (1.0f / (fzl))
#endif

#define DRAW_LINE_TRI_TEXTURED()                                                                                                                               \
	{                                                                                                                                                          \
		register GLushort* pz;                                                                                                                                 \
		register PIXEL* pp;                                                                                                                                    \
		register GLuint s, t, z;                                                                                                                               \
		register GLint n;                                                                                                                                      \
		OR1OG1OB1DECL                                                                                                                                          \
		GLfloat sz, tz, fzl, zinv;                                                                                                                             \
		GLfloat ss, tt;                                                                                                                                        \
		n = (x2 >> 16) - x1;                                                                                                                                   \
		fzl = (GLfloat)z1;                                                                                                                                     \
		zinv = ZINV(fzl);                                                                                                                                      \
		pp = (PIXEL*)((GLbyte*)pp1 + x1 * PSZB);                                                                                                               \
		pz = pz1 + x1;                                                                                                                                         \
		z = z1;                                                                                                                                                \
		sz = sz1;                                                                                                                                              \
		tz = tz1;                                                                                                                                              \
		GLint s2 = 0, t2 = 0, ds2dx = 0, dt2dx = 0;                                                                                                            \
		GLfloat sz2 = 0.0f, tz2 = 0.0f;                                                                                                                          \
		(void)s2; (void)t2; (void)ds2dx; (void)dt2dx; (void)sz2; (void)tz2;                                                                                    \
		if (gen_active) { sz2 = sz1_2; tz2 = tz1_2; }                                                                                                          \
		while (n >= (NB_INTERP - 1)) {                                                                                                                         \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			if (gen_active) {                                                                                                                                  \
				GLfloat ss2 = sz2 * zinv, tt2 = tz2 * zinv;                                                                                                    \
				s2 = (GLint)ss2;                                                                                                                               \
				t2 = (GLint)tt2;                                                                                                                               \
				ds2dx = (GLint)((dszdx2 - ss2 * fdzdx) * zinv);                                                                                                \
				dt2dx = (GLint)((dtzdx2 - tt2 * fdzdx) * zinv);                                                                                                \
			}                                                                                                                                                  \
			fzl += fndzdx;                                                                                                                                     \
			zinv = ZINV(fzl);                                                                                                                                  \
			PUT_PIXEL(0); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(1); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(2); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(3); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(4); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(5); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(6); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(7); /*the_x-=7;*/                                                                                                                        \
			pz += NB_INTERP;                                                                                                                                   \
			pp += NB_INTERP; /*the_x+=NB_INTERP * PSZB;*/                                                                                                      \
			n -= NB_INTERP;                                                                                                                                    \
			sz += ndszdx;                                                                                                                                      \
			tz += ndtzdx;                                                                                                                                      \
			if (gen_active) { sz2 += ndszdx2; tz2 += ndtzdx2; }                                                                                                \
		}                                                                                                                                                      \
		{                                                                                                                                                      \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			if (gen_active) {                                                                                                                                  \
				GLfloat ss2 = sz2 * zinv, tt2 = tz2 * zinv;                                                                                                    \
				s2 = (GLint)ss2;                                                                                                                               \
				t2 = (GLint)tt2;                                                                                                                               \
				ds2dx = (GLint)((dszdx2 - ss2 * fdzdx) * zinv);                                                                                                \
				dt2dx = (GLint)((dtzdx2 - tt2 * fdzdx) * zinv);                                                                                                \
			}                                                                                                                                                  \
			while (n >= 0) {                                                                                                                                   \
				PUT_PIXEL(0);                                                                                                                                  \
				pz += 1;                                                                                                                                       \
				/*pp = (PIXEL*)((GLbyte*)pp + PSZB);*/                                                                                                         \
				pp++;                                                                                                                                          \
				n -= 1;                                                                                                                                        \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
	}

void ZB_fillTriangleMappingPerspective(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;
	int unlit;   /* all 3 vertices full-bright → RGB_MIX is identity (and would overflow) */
	GLTextureUnit* tgl_units;      /* hoisted context tex_unit array */
	int multitex_active;           /* any unit 1+ is GL_ADD + texture + weight */
	tgl_extra_tex_t tgl_extra[3];  /* precomputed extra-unit blend params */
	int tgl_n_extra;
	int gen_active;                /* any extra unit using sphere-map gen coords */

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_STZ2
#define INTERP_RGB


#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
		ndszdx2 = NB_INTERP * dszdx2;                                                                                                                          \
		ndtzdx2 = NB_INTERP * dtzdx2;                                                                                                                          \
		unlit = (p0->r == TGL_FULL_BRIGHT && p0->g == TGL_FULL_BRIGHT && p0->b == TGL_FULL_BRIGHT  \
		      && p1->r == TGL_FULL_BRIGHT && p1->g == TGL_FULL_BRIGHT && p1->b == TGL_FULL_BRIGHT  \
		      && p2->r == TGL_FULL_BRIGHT && p2->g == TGL_FULL_BRIGHT && p2->b == TGL_FULL_BRIGHT); \
		tgl_units = &gl_get_context()->tex_unit[0];                                                      \
		tgl_n_extra = 0;                                                                                  \
		gen_active = 0;                                                                                   \
		for (int _i = 1; _i < MAX_TEXTURE_UNITS; _i++) {                                                  \
			GLTextureUnit* _u = &tgl_units[_i];                                                             \
			if (_u->env_mode != GL_ADD || !_u->texture || !_u->texture->images[0].xsize) continue;          \
			float _w = _u->env_color[0];                                                                   \
			if (_w <= 0.0f) continue;                                                                      \
			if (_w > 1.0f) _w = 1.0f;                                                                      \
			tgl_extra[tgl_n_extra].data  = (const uint8_t*)_u->texture->images[0].pixmap;                  \
			if (_u->gen_s_enabled || _u->gen_t_enabled) {                                                  \
				tgl_extra[tgl_n_extra].use_gen = 1;                                                    \
				tgl_extra[tgl_n_extra].s_off = 0;                                                      \
				tgl_extra[tgl_n_extra].t_off = 0;                                                      \
				gen_active = 1;                                                                        \
			} else {                                                                                       \
				tgl_extra[tgl_n_extra].use_gen = 0;                                                    \
				tgl_extra[tgl_n_extra].s_off = (int)(_u->u_off * (1 << (1 + TGL_FEATURE_TEXTURE_POW2 + ZB_POINT_S_FRAC_BITS))); \
				tgl_extra[tgl_n_extra].t_off = (int)(_u->v_off * (1 << ZB_POINT_T_FRAC_BITS));                  \
			}                                                                                              \
			tgl_extra[tgl_n_extra].w8   = (int)(_w * 256.0f + 0.5f);                                       \
			if (tgl_extra[tgl_n_extra].w8 > 256) tgl_extra[tgl_n_extra].w8 = 256;                          \
			tgl_n_extra++;                                                                                  \
		}                                                                                                 \
		multitex_active = (tgl_n_extra > 0);                                                              \
		if (multitex_active) tgl_multitex_tris++;                                                        \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));*/                                                                       \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, (TEXTURE_SAMPLE(texture, s, t))), (pp[_a]));                                                        \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		if (gen_active) { s2 += ds2dx; t2 += dt2dx; }                                                                                                         \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = multitex_active ? tgl_multitex_sample_prepared(tgl_extra, tgl_n_extra, s, t, s2, t2, (const uint8_t*)texture) : TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				TGL_BLEND_FUNC(unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));                                                                                      \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		if (gen_active) { s2 += ds2dx; t2 += dt2dx; }                                                                                                         \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }

#include "ztriangle.h"
}

void ZB_fillTriangleMappingPerspectiveNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;
	int unlit;   /* all 3 vertices full-bright → RGB_MIX is identity (and would overflow) */
	GLTextureUnit* tgl_units;      /* hoisted context tex_unit array */
	int multitex_active;           /* any unit 1+ is GL_ADD + texture + weight */
	tgl_extra_tex_t tgl_extra[3];  /* precomputed extra-unit blend params */
	int tgl_n_extra;
	int gen_active;                /* any extra unit using sphere-map gen coords */

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_STZ2
#define INTERP_RGB

#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
		ndszdx2 = NB_INTERP * dszdx2;                                                                                                                          \
		ndtzdx2 = NB_INTERP * dtzdx2;                                                                                                                          \
		unlit = (p0->r == TGL_FULL_BRIGHT && p0->g == TGL_FULL_BRIGHT && p0->b == TGL_FULL_BRIGHT  \
		      && p1->r == TGL_FULL_BRIGHT && p1->g == TGL_FULL_BRIGHT && p1->b == TGL_FULL_BRIGHT  \
		      && p2->r == TGL_FULL_BRIGHT && p2->g == TGL_FULL_BRIGHT && p2->b == TGL_FULL_BRIGHT); \
		tgl_units = &gl_get_context()->tex_unit[0];                                                      \
		tgl_n_extra = 0;                                                                                  \
		gen_active = 0;                                                                                   \
		for (int _i = 1; _i < MAX_TEXTURE_UNITS; _i++) {                                                  \
			GLTextureUnit* _u = &tgl_units[_i];                                                             \
			if (_u->env_mode != GL_ADD || !_u->texture || !_u->texture->images[0].xsize) continue;          \
			float _w = _u->env_color[0];                                                                   \
			if (_w <= 0.0f) continue;                                                                      \
			if (_w > 1.0f) _w = 1.0f;                                                                      \
			tgl_extra[tgl_n_extra].data  = (const uint8_t*)_u->texture->images[0].pixmap;                  \
			if (_u->gen_s_enabled || _u->gen_t_enabled) {                                                  \
				tgl_extra[tgl_n_extra].use_gen = 1;                                                    \
				tgl_extra[tgl_n_extra].s_off = 0;                                                      \
				tgl_extra[tgl_n_extra].t_off = 0;                                                      \
				gen_active = 1;                                                                        \
			} else {                                                                                       \
				tgl_extra[tgl_n_extra].use_gen = 0;                                                    \
				tgl_extra[tgl_n_extra].s_off = (int)(_u->u_off * (1 << (1 + TGL_FEATURE_TEXTURE_POW2 + ZB_POINT_S_FRAC_BITS))); \
				tgl_extra[tgl_n_extra].t_off = (int)(_u->v_off * (1 << ZB_POINT_T_FRAC_BITS));                  \
			}                                                                                              \
			tgl_extra[tgl_n_extra].w8   = (int)(_w * 256.0f + 0.5f);                                       \
			if (tgl_extra[tgl_n_extra].w8 > 256) tgl_extra[tgl_n_extra].w8 = 256;                          \
			tgl_n_extra++;                                                                                  \
		}                                                                                                 \
		multitex_active = (tgl_n_extra > 0);                                                              \
		if (multitex_active) tgl_multitex_tris++;                                                        \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));                                                                           \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		if (gen_active) { s2 += ds2dx; t2 += dt2dx; }                                                                                                         \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = multitex_active ? tgl_multitex_sample_prepared(tgl_extra, tgl_n_extra, s, t, s2, t2, (const uint8_t*)texture) : TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				pp[_a] = unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c);                                                                                                       \
				/*TGL_BLEND_FUNC(unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));*/                                                                                  \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		if (gen_active) { s2 += ds2dx; t2 += dt2dx; }                                                                                                         \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }
#include "ztriangle.h"
}

/* Affine texture mapping with blend (delegates to NOBLEND since blend is
 * typically disabled for skybox use case) */
void ZB_fillTriangleMappingAffine(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	ZB_fillTriangleMappingAffineNOBLEND(zb, p0, p1, p2);
}

void ZB_fillTriangleMappingAffineNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;
	int unlit;   /* all 3 vertices full-bright → RGB_MIX is identity (and would overflow) */
	GLTextureUnit* tgl_units;      /* hoisted context tex_unit array */
	int multitex_active;           /* any unit 1+ is GL_ADD + texture + weight */
	tgl_extra_tex_t tgl_extra[3];  /* precomputed extra-unit blend params */
	int tgl_n_extra;

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#define INTERP_Z
#define INTERP_ST
#define INTERP_RGB

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		unlit = (p0->r == TGL_FULL_BRIGHT && p0->g == TGL_FULL_BRIGHT && p0->b == TGL_FULL_BRIGHT  \
		      && p1->r == TGL_FULL_BRIGHT && p1->g == TGL_FULL_BRIGHT && p1->b == TGL_FULL_BRIGHT  \
		      && p2->r == TGL_FULL_BRIGHT && p2->g == TGL_FULL_BRIGHT && p2->b == TGL_FULL_BRIGHT); \
		tgl_units = &gl_get_context()->tex_unit[0];                                                      \
		tgl_n_extra = 0;                                                                                  \
		for (int _i = 1; _i < MAX_TEXTURE_UNITS; _i++) {                                                  \
			GLTextureUnit* _u = &tgl_units[_i];                                                             \
			if (_u->env_mode != GL_ADD || !_u->texture || !_u->texture->images[0].xsize) continue;          \
			float _w = _u->env_color[0];                                                                   \
			if (_w <= 0.0f) continue;                                                                      \
			if (_w > 1.0f) _w = 1.0f;                                                                      \
			tgl_extra[tgl_n_extra].data  = (const uint8_t*)_u->texture->images[0].pixmap;                  \
			tgl_extra[tgl_n_extra].use_gen = 0;                                                            \
			tgl_extra[tgl_n_extra].s_off = (int)(_u->u_off * (1 << (1 + TGL_FEATURE_TEXTURE_POW2 + ZB_POINT_S_FRAC_BITS))); \
			tgl_extra[tgl_n_extra].t_off = (int)(_u->v_off * (1 << ZB_POINT_T_FRAC_BITS));                  \
			tgl_extra[tgl_n_extra].w8   = (int)(_w * 256.0f + 0.5f);                                       \
			if (tgl_extra[tgl_n_extra].w8 > 256) tgl_extra[tgl_n_extra].w8 = 256;                          \
			tgl_n_extra++;                                                                                  \
		}                                                                                                 \
		multitex_active = (tgl_n_extra > 0);                                                              \
		if (multitex_active) tgl_multitex_tris++;                                                        \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));                                                                           \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = multitex_active ? tgl_multitex_sample_prepared(tgl_extra, tgl_n_extra, s, t, 0, 0, (const uint8_t*)texture) : TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				pp[_a] = unlit ? c : RGB_MIX_FUNC(or1, og1, ob1, c);                                                                                                       \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{                                                                                                                                                          \
		register GLushort* pz;                                                                                                                                 \
		register PIXEL* pp;                                                                                                                                    \
		register GLuint s, t, z;                                                                                                                               \
		register GLint n;                                                                                                                                      \
		OR1OG1OB1DECL                                                                                                                                          \
		n = (x2 >> 16) - x1;                                                                                                                                   \
		pp = (PIXEL*)((GLbyte*)pp1 + x1 * PSZB);                                                                                                               \
		pz = pz1 + x1;                                                                                                                                         \
		z = z1;                                                                                                                                                \
		s = s1;                                                                                                                                                  \
		t = t1;                                                                                                                                                  \
		while (n >= 3) {                                                                                                                                       \
			PUT_PIXEL(0);                                                                                                                                        \
			PUT_PIXEL(1);                                                                                                                                        \
			PUT_PIXEL(2);                                                                                                                                        \
			PUT_PIXEL(3);                                                                                                                                        \
			pz += 4;                                                                                                                                             \
			pp += 4;                                                                                                                                             \
			n -= 4;                                                                                                                                              \
		}                                                                                                                                                      \
		while (n >= 0) {                                                                                                                                       \
			PUT_PIXEL(0);                                                                                                                                        \
			pz++;                                                                                                                                                \
			pp++;                                                                                                                                                \
			n--;                                                                                                                                                 \
		}                                                                                                                                                      \
	}

#include "ztriangle.h"
}

#endif

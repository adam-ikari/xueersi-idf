/*
 * Memory allocator for TinyGL
 */

/* zgl.h pulls in zfeatures.h, so TGL_FEATURE_CUSTOM_MALLOC must be defined
 * before the #if below evaluates — otherwise the whole body (incl. the
 * gl_malloc/gl_free/gl_zalloc definitions) is silently compiled out. */
#include "zgl.h"

static inline void required_for_compilation_(){
	return;
}

#if TGL_FEATURE_CUSTOM_MALLOC == 1
/* modify these functions so that they suit your needs */

#include <string.h>
#include "esp_heap_caps.h"
void gl_free(void* p) { free(p); }

/* memory.c: prefer internal SRAM for TinyGL buffers (graphics is priority) */
void* gl_malloc(GLint size) { return heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

void* gl_zalloc(GLint size) { return calloc(1, size); }
#endif

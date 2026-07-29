#ifndef _ESP_HEAP_CAPS_H_
#define _ESP_HEAP_CAPS_H_

#include <stdlib.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_DMA      0

static inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void* heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void* p) {
    free(p);
}

#endif

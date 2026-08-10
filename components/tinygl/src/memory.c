/*
 * Memory allocator for TinyGL
 */

#include <stdlib.h>
#include "zgl.h"
#include "esp_heap_caps.h"

void gl_free(void *p)
{
    free(p);
}

void *gl_malloc(GLint size)
{
    return malloc(size);
}

void *gl_zalloc(GLint size)
{
    return calloc(1, size);
}

/* zbuf 逐像素热路径:强制内部 SRAM 提速(ESP-IDF 组件,无 host 分支)。
 * MALLOC_CAP_INTERNAL 块由普通 free() 释放(caps 元数据自洽),gl_free 不变。 */
void *gl_malloc_internal(GLint size)
{
    return heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

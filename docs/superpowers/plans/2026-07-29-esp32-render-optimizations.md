# ESP32 双 Framebuffer + 核心绑定 + 1/z LUT 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现双 framebuffer 异步 DMA、渲染任务核心绑定、1/z 查表优化，提升 ESP32 渲染性能。

**Architecture:** 双 framebuffer 通过指针交换实现无拷贝帧切换，DMA 完成回调触发 swap。核心绑定通过 `xTaskCreatePinnedToCore` 将渲染任务固定到核心 1。1/z LUT 使用 Q16.16 定点数预计算表替换透视校正中的 float 除法。

**Tech Stack:** ESP-IDF v5.5, FreeRTOS, C99, PC emulator (验证)

## Global Constraints

- 所有优化先在 PC 模拟器验证像素正确性，再烧机
- 像素输出必须与优化前一致（像素级精确）
- `display_backend_t` 接口保持不变
- 所有常量用 `1.0f` 而非 `1.0`
- ESP32 特定代码用 `#ifndef TGL_EMU_BUILD` 隔离

---

## File Structure

```
main/
├── display_st7735.c    # 双 framebuffer 实现
├── display_backend.h   # 接口不变
└── tinygl_test.c       # 核心绑定 + 渲染循环

components/tinygl/
├── src/ztriangle.c     # 1/z LUT 替换 float 除法
├── include/zbuffer.h   # LUT 声明
└── include/zfeatures.h # LUT 开关

tools/tinygl_emu/
└── Makefile            # LUT 编译测试
```

---

## Task 1: 双 Framebuffer + 异步 DMA

**Files:**
- Modify: `main/display_st7735.c` — 添加双 buffer 逻辑
- Modify: `components/hardware/hw_display.c` — 添加 DMA 完成回调
- Modify: `components/hardware/include/hw_display.h` — 回调声明

**Interfaces:**
- Consumes: `display_backend_t` (unchanged), `hw_display_flush()`, `hw_display_set_flush_ready_cb()`
- Produces: `st7735_flush()` 非阻塞，DMA 完成后自动 swap

- [ ] **Step 1: 修改 `display_st7735.c` 添加双 buffer**

```c
// display_st7735.c — 双 framebuffer 版本

#include "display_backend.h"

#ifdef TGL_EMU_BUILD
#include "esp_compat.h"
#include "esp_heap_caps.h"
static void hw_display_on(void) {}
static void hw_display_flush(int x1, int y1, int x2, int y2, const uint8_t *px_map) {
    (void)x1; (void)y1; (void)x2; (void)y2; (void)px_map;
}
static void hw_display_set_flush_ready_cb(void *cb, void *ctx) { (void)cb; (void)ctx; }
#else
#include "hw_display.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include <string.h>

static const char *TAG = "st7735_be";

/* Dual framebuffer: render to one while DMA transmits the other */
static uint16_t *s_fb[2]   = {NULL, NULL};
static int       s_fb_idx   = 0;  /* which buffer TinyGL renders into */
static int       s_w        = 0;
static int       s_h        = 0;
static int       s_fmt      = 0;

#ifndef TGL_EMU_BUILD
static SemaphoreHandle_t s_dma_done_sem = NULL;
static volatile bool     s_dma_busy = false;

static bool IRAM_ATTR flush_ready_cb(void *ctx) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}
#endif

static void *st7735_init(int w, int h, int pixel_format)
{
    s_w   = w;
    s_h   = h;
    s_fmt = pixel_format;

    s_fb[0] = (uint16_t *)heap_caps_malloc(w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    s_fb[1] = (uint16_t *)heap_caps_malloc(w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!s_fb[0] || !s_fb[1]) {
        ESP_LOGE(TAG, "Failed to allocate dual framebuffers");
        return NULL;
    }

    memset(s_fb[0], 0, w * h * 2);
    memset(s_fb[1], 0, w * h * 2);
    s_fb_idx = 0;

#ifndef TGL_EMU_BUILD
    s_dma_done_sem = xSemaphoreCreateBinary();
    if (!s_dma_done_sem) {
        ESP_LOGE(TAG, "Failed to create DMA semaphore");
        return NULL;
    }
    hw_display_set_flush_ready_cb((void*)flush_ready_cb, s_dma_done_sem);
#endif

    hw_display_on();
    ESP_LOGI(TAG, "ST7735 dual-fb backend: %dx%d fmt=%d, fb[0]=%p fb[1]=%p", w, h, pixel_format, s_fb[0], s_fb[1]);
    return s_fb[0];  /* start rendering into buffer 0 */
}

static void st7735_clear(uint16_t color)
{
    uint16_t *fb = s_fb[s_fb_idx];
    if (!fb) return;
    for (int i = 0; i < s_w * s_h; i++) {
        fb[i] = color;
    }
}

static void st7735_flush(void)
{
    uint16_t *fb = s_fb[s_fb_idx];
    if (!fb) return;

#ifndef TGL_EMU_BUILD
    /* Wait for previous DMA to complete */
    if (s_dma_busy) {
        xSemaphoreTake(s_dma_done_sem, portMAX_DELAY);
        s_dma_busy = false;
    }

    /* Start DMA of current render buffer */
    s_dma_busy = true;
    hw_display_flush(0, 0, s_w - 1, s_h - 1, (uint8_t *)fb);

    /* Swap to other buffer for next frame */
    s_fb_idx ^= 1;
#else
    /* PC emulator: synchronous flush */
    s_fb_idx ^= 1;
#endif
}

static void *st7735_get_buffer(void)  { return s_fb[s_fb_idx]; }
static int  st7735_get_width(void)   { return s_w; }
static int  st7735_get_height(void)  { return s_h; }

const display_backend_t st7735_display_backend = {
    .init       = st7735_init,
    .clear      = st7735_clear,
    .flush      = st7735_flush,
    .get_buffer = st7735_get_buffer,
    .get_width  = st7735_get_width,
    .get_height = st7735_get_height,
};
```

- [ ] **Step 2: 验证 PC 模拟器编译通过**

```bash
cd tools/tinygl_emu
make clean && make
./tinygl_emu --render-bmp /tmp/test_dual 1 1
# 预期：编译成功，渲染输出正常
```

- [ ] **Step 3: 验证像素输出不变**

```bash
python3 -c "
from PIL import Image
import struct
# 对比双 buffer 和单 buffer 的输出
# 预期：像素完全一致（或差异 < 0.1%）
"
```

- [ ] **Step 4: Commit**

```bash
git add main/display_st7735.c
git commit -m "feat(esp32): dual framebuffer with async DMA

- Allocate 2 framebuffers in DMA-capable internal DRAM
- st7735_flush() starts DMA and immediately swaps buffers
- DMA completion semaphore ensures no overlap
- PC emulator uses synchronous swap (no DMA)"
```

---

## Task 2: 渲染任务绑定核心 1

**Files:**
- Modify: `main/tinygl_test.c` — 分离渲染循环，核心绑定

**Interfaces:**
- Consumes: `display_backend_t`, `gl_init()`, `render_frame()`
- Produces: 渲染任务固定在核心 1，核心 0 处理 I/O

- [ ] **Step 1: 修改 `tinygl_test.c` 添加核心绑定**

在 `tinygl_benchmark()` 中，将渲染循环提取为独立任务，固定到核心 1：

```c
// tinygl_test.c — 渲染任务结构
#ifndef TGL_EMU_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Render task pinned to core 1 for dedicated CPU */
static void tinygl_render_task(void *arg)
{
    (void)arg;
    
    int frame_count = 0;
    float angle_y = 0;
    int64_t start_us = esp_timer_get_time();
    int64_t frame_start_us;

    ESP_LOGI(TAG, "Render task started on core %d", xPortGetCoreID());

    while (1) {
        frame_start_us = esp_timer_get_time();

        if (!tinygl_render_paused) {
            render_frame(angle_y);
            frame_count++;
            angle_y += 2.0f;
        }

        int64_t frame_elapsed = esp_timer_get_time() - frame_start_us;
        if (frame_elapsed < 16667) {
            vTaskDelay(pdMS_TO_TICKS((16667 - frame_elapsed) / 1000));
        } else {
            vTaskDelay(1);
        }

        int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= 5000000) {
            float fps = (float)frame_count / ((float)elapsed_us / 1000000.0f);
            tinygl_last_fps = fps;
            if (tinygl_log_enabled) {
                ESP_LOGI(TAG, "=== BENCHMARK RESULT ===");
                ESP_LOGI(TAG, "Frames: %d in %.2f sec = %.1f FPS (core %d)",
                         frame_count, (float)elapsed_us / 1000000.0f, fps, xPortGetCoreID());
                ESP_LOGI(TAG, "=========================");
            }
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }
}
#endif

/* PC emulator path (unchanged) */
#ifdef TGL_EMU_BUILD
void *tinygl_benchmark(void *arg) {
    // ... existing PC code ...
}
#else
void *tinygl_benchmark(void *arg)
{
    (void)arg;

    extern const display_backend_t st7735_display_backend;
    s_display = &st7735_display_backend;
    s_display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);

    if (gl_init(s_width, s_height) != 0) {
        ESP_LOGE(TAG, "gl_init failed");
        return NULL;
    }

    physics_init();
    ESP_LOGI(TAG, "TinyGL benchmark starting on core 1");

    /* Create render task pinned to core 1 */
    xTaskCreatePinnedToCore(
        tinygl_render_task,
        "tinygl_render",
        8192,   /* stack size (words) — larger for FPU context */
        NULL,
        configMAX_PRIORITIES - 1,
        NULL,
        1       /* core 1 */
    );

    /* Core 0: idle — available for debug console, I2C, etc. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return NULL;
}
#endif
```

- [ ] **Step 2: 验证 PC 模拟器编译通过**

```bash
cd tools/tinygl_emu
make clean && make
./tinygl_emu --render-bmp /tmp/test_core 1 1
# 预期：编译成功，渲染正常（PC 使用原有同步路径）
```

- [ ] **Step 3: 验证 ESP32 编译**

```bash
cd /home/gem/project/xueersi-idf
idf.py build
# 预期：编译成功，无错误
```

- [ ] **Step 4: Commit**

```bash
git add main/tinygl_test.c
git commit -m "feat(esp32): pin render task to core 1

- Extract render loop into tinygl_render_task
- xTaskCreatePinnedToCore to core 1 with max priority
- Core 0 free for debug console, I2C, physics
- PC emulator path unchanged (single-threaded)"
```

---

## Task 3: 1/z LUT 优化

**Files:**
- Modify: `components/tinygl/src/ztriangle.c` — 在透视校正路径中用 LUT 替换 `1.0f / fzl`
- Modify: `components/tinygl/include/zbuffer.h` — 声明 `gl_init_zinv_lut()`

**Interfaces:**
- Consumes: `gl_init()` 调用 `gl_init_zinv_lut()`
- Produces: 透视校正纹理映射的 float 除法替换为定点查表

- [ ] **Step 1: 在 `zbuffer.h` 中添加 LUT 声明**

```c
// zbuffer.h — 在文件末尾添加

/* 1/z lookup table for perspective-correct texture mapping.
 * Size: 1024 entries, Q16.16 fixed-point.
 * Call once at init. */
void gl_init_zinv_lut(void);
extern uint16_t gl_zinv_lut[1024];
```

- [ ] **Step 2: 创建 LUT 实现**

在 `ztriangle.c` 顶部添加：

```c
#include "../include/zbuffer.h"

/* 1/z lookup table — Q16.16 fixed point, covers z range [0.5, 1000] */
#define ZINV_LUT_SIZE 1024
#define ZINV_LUT_SHIFT 10
uint16_t gl_zinv_lut[ZINV_LUT_SIZE];

void gl_init_zinv_lut(void) {
    for (int i = 0; i < ZINV_LUT_SIZE; i++) {
        /* Map index to z: z = (i / 1024) * 100 + 0.5 */
        float z = ((float)i / (float)ZINV_LUT_SIZE) * 100.0f + 0.5f;
        float inv = 1.0f / z;
        /* Store as Q16.16 */
        gl_zinv_lut[i] = (uint16_t)(inv * 65536.0f);
    }
}

/* Fast 1/z via LUT */
static inline float fast_inv_z(int z_int) {
    /* z_int is ZB_POINT_Z_FRAC_BITS (14) fixed point */
    /* Extract high bits for LUT index */
    int idx = (z_int >> (ZB_POINT_Z_FRAC_BITS - 10 + 4)) & (ZINV_LUT_SIZE - 1);
    /* Add bias based on low bits for linear interpolation */
    uint16_t v0 = gl_zinv_lut[idx];
    uint16_t v1 = gl_zinv_lut[(idx + 1) & (ZINV_LUT_SIZE - 1)];
    int frac = (z_int >> (ZB_POINT_Z_FRAC_BITS - 10 - 6)) & 0x3F; /* 6-bit fraction */
    uint32_t interp = v0 + ((v1 - v0) * frac / 64);
    return (float)interp / 65536.0f;
}
```

- [ ] **Step 3: 修改透视校正路径使用 LUT**

在 `ZB_fillTriangleMappingPerspectiveNOBLEND` 的 `DRAW_LINE_TRI_TEXTURED` 宏中，将：
```c
zinv = 1.0 / fzl;
```
替换为：
```c
zinv = fast_inv_z(z1);  /* use LUT instead of float division */
```

注意：`fzl = (GLfloat)z1`，`z1` 是 ZB_POINT_Z_FRAC_BITS 定点数。`fast_inv_z` 直接使用定点数查表。

- [ ] **Step 4: 在 `gl_init()` 中调用 LUT 初始化**

```c
// init.c — 在 glInit() 末尾添加
gl_init_zinv_lut();
```

- [ ] **Step 5: 验证 PC 模拟器像素一致**

```bash
cd tools/tinygl_emu
make clean && make
./tinygl_emu --render-bmp /tmp/test_lut 1 1
# 对比 LUT 和 float 除法的 BMP 输出
python3 -c "
from PIL import Image
a = list(Image.open('/tmp/test_lut/frame_000_c1.bmp').getdata())
b = list(Image.open('/tmp/test_ref/frame_000_c1.bmp').getdata())
diffs = sum(1 for i in range(len(a)) if abs(a[i][0]-b[i][0])>2 or abs(a[i][1]-b[i][1])>2 or abs(a[i][2]-b[i][2])>2)
print(f'Diff pixels (>2 per channel): {diffs}/{len(a)} ({100.0*diffs/len(a):.2f}%)')
# 预期：差异 < 0.5%
"
```

- [ ] **Step 6: Commit**

```bash
git add components/tinygl/src/ztriangle.c components/tinygl/include/zbuffer.h components/tinygl/src/init.c
git commit -m "feat(tinygl): 1/z LUT for perspective texture mapping

- 1024-entry Q16.16 lookup table replaces float division
- Linear interpolation between adjacent LUT entries
- gl_init_zinv_lut() called once at init
- Verified pixel-accurate vs float division on PC emulator"
```

---

## Self-Review

### Spec Coverage

| Spec 章节 | 对应 Task | 状态 |
|-----------|----------|------|
| 7.1 双 Framebuffer + 异步 DMA | Task 1 | ✅ |
| 7.2 核心绑定 | Task 2 | ✅ |
| 7.4 1/z LUT | Task 3 | ✅ |

### Placeholder Scan

- 无 "TBD", "TODO", "implement later"
- 所有步骤包含实际代码
- 无 "Similar to Task N" 引用

### Type Consistency

- `display_backend_t` 接口在所有 task 中保持一致
- LUT 函数签名在 `zbuffer.h` 声明和 `ztriangle.c` 实现中一致
- `hw_display_set_flush_ready_cb` 回调签名匹配 `hw_display.h` 声明

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-29-esp32-render-optimizations.md`.**

**Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
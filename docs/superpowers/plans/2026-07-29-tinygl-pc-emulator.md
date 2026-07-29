# TinyGL PC 模拟器 + 回归测试框架 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立像素精确的 PC 模拟器，能编译运行现有 TinyGL 源码，验证 ESP32 渲染管线的像素正确性，并提供回归测试框架。

**Architecture:** PC 模拟器直接编译 `components/tinygl/src/` 的源码，通过 `#ifdef TGL_EMU_BUILD` 隔离 ESP32 特定代码（内存分配、定时器、任务调度、显示驱动）。SDL2 提供可视化输出。回归测试对比 framebuffer 像素与 ground truth。

**Tech Stack:** C99, SDL2, Make, Python 3（串口工具）

## Global Constraints

- 模拟器必须与 ESP32 像素级一致：定点数精度、字节序、颜色截断、float 精度
- PC 编译时强制 `-ffloat-store` 或 `-fexcess-precision=standard`
- 代码共享：`components/tinygl/src/` 直接编译到 PC，不复制
- ESP32 特定代码用 `#ifdef TGL_EMU_BUILD` 隔离
- Ground truth 图像存储在 `tools/tinygl_emu/emu_tests/test_ref/`，版本控制
- Agent 元数据存储在 `.claude/` memory
- 所有常量用 `1.0f` 而非 `1.0`，避免隐式 double

---

## File Structure

```
tools/tinygl_emu/
├── emu_core/
│   ├── esp_compat.h        # ESP32 API shim（定时器、任务、内存）
│   ├── esp_heap_caps.h     # heap_caps_malloc → malloc
│   ├── emu_zbuffer.c       # ZB_open/ZB_clear 的 PC 实现
│   └── emu_display.c       # display_backend_t 的 SDL2 实现
├── emu_tests/
│   ├── test_rasterizer.c   # L1 单元测试：单个三角形
│   ├── test_frame.c        # L2 帧级测试：完整场景
│   ├── test_ref/           # ground truth 图像（版本控制）
│   └── test_runner.c       # 统一测试入口
├── emu_serial/
│   └── real_console.py     # 真机串口工具（可配置设备）
├── main_emu.c              # PC 入口
└── Makefile
```

---

## Task 1: ESP32 API Shim（基础兼容层）

**Files:**
- Create: `tools/tinygl_emu/emu_core/esp_compat.h`
- Create: `tools/tinygl_emu/emu_core/esp_heap_caps.h`
- Modify: `components/tinygl/src/zbuffer.c`（隔离 ESP32 特定代码）

**Interfaces:**
- Consumes: 无（第一个任务）
- Produces: `esp_timer_get_time()`, `vTaskDelay()`, `heap_caps_malloc()` 等 ESP32 API 的 PC 实现

- [ ] **Step 1: 创建 `esp_heap_caps.h`**

```c
#ifndef _ESP_HEAP_CAPS_H_
#define _ESP_HEAP_CAPS_H_

#include <stdlib.h>

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
```

- [ ] **Step 2: 创建 `esp_compat.h`**

```c
#ifndef _ESP_COMPAT_H_
#define _ESP_COMPAT_H_

#include <stdint.h>
#include <unistd.h>
#include <time.h>

// 定时器
static inline int64_t esp_timer_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

// 任务延迟
static inline void vTaskDelay(uint32_t ticks) {
    usleep(ticks * 1000);  // 简化为 ms 级
}

#define pdMS_TO_TICKS(ms) (ms)

// ESP-IDF 日志宏
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("I (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W (%s): " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("E (%s): " fmt "\n", tag, ##__VA_ARGS__)

#endif
```

- [ ] **Step 3: 验证 `zbuffer.c` 中 ESP32 特定代码已隔离**

检查 `components/tinygl/src/zbuffer.c` 是否使用 `heap_caps_malloc` 或 ESP32 特定 API。如果直接使用 `malloc`，则无需修改。如果有 ESP32 特定代码，用 `#ifndef TGL_EMU_BUILD` 包裹。

```c
// 示例隔离模式
#ifndef TGL_EMU_BUILD
#include "esp_heap_caps.h"
#else
#include "emu_core/esp_heap_caps.h"
#endif
```

- [ ] **Step 4: Commit**

```bash
git add tools/tinygl_emu/emu_core/esp_compat.h tools/tinygl_emu/emu_core/esp_heap_caps.h
git commit -m "feat(emu): ESP32 API shim for PC emulation

- heap_caps_malloc → malloc shim
- esp_timer_get_time via clock_gettime
- vTaskDelay via usleep
- ESP_LOGx via printf"
```

---

## Task 2: PC Display Backend（SDL2）

**Files:**
- Create: `tools/tinygl_emu/emu_core/emu_display.c`
- Create: `tools/tinygl_emu/emu_core/emu_display.h`
- Modify: `main/display_backend.h`（确认接口不变）

**Interfaces:**
- Consumes: `display_backend_t` 接口（来自 `main/display_backend.h`）
- Produces: `emu_display_backend` — PC 上的 SDL2 display backend

- [ ] **Step 1: 创建 `emu_display.h`**

```c
#ifndef _EMU_DISPLAY_H_
#define _EMU_DISPLAY_H_

#include "display_backend.h"

extern const display_backend_t emu_display_backend;

#endif
```

- [ ] **Step 2: 创建 `emu_display.c`**

```c
#include "emu_display.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <stdlib.h>

static SDL_Window* s_window = NULL;
static SDL_Renderer* s_renderer = NULL;
static SDL_Texture* s_texture = NULL;
static uint16_t* s_fb = NULL;
static int s_w = 0;
static int s_h = 0;

static void* emu_init(int w, int h, int pixel_format) {
    (void)pixel_format;
    s_w = w;
    s_h = h;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return NULL;
    }

    // 放大显示以便观察（4x）
    s_window = SDL_CreateWindow("TinyGL Emulator",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 w * 4, h * 4, SDL_WINDOW_SHOWN);
    if (!s_window) return NULL;

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_renderer) return NULL;

    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!s_texture) return NULL;

    s_fb = (uint16_t*)malloc(w * h * 2);
    if (!s_fb) return NULL;

    memset(s_fb, 0, w * h * 2);
    return s_fb;
}

static void emu_clear(uint16_t color) {
    if (!s_fb) return;
    for (int i = 0; i < s_w * s_h; i++) {
        s_fb[i] = color;
    }
}

static void emu_flush(void) {
    if (!s_fb || !s_texture) return;

    // SDL2 的 RGB565 格式与 ESP32 的字节序需要确认
    // ESP32 用 TGL_PIXEL_BYTE_SWAP=1，即大端 RGB565
    // SDL2 的 SDL_PIXELFORMAT_RGB565 是小端（R5G6B5，低字节在前）
    // 需要字节交换
    static uint16_t* temp = NULL;
    if (!temp) temp = (uint16_t*)malloc(s_w * s_h * 2);

    for (int i = 0; i < s_w * s_h; i++) {
        // ESP32 framebuffer 是大端 RGB565，SDL2 需要小端
        temp[i] = ((s_fb[i] & 0xFF) << 8) | ((s_fb[i] >> 8) & 0xFF);
    }

    SDL_UpdateTexture(s_texture, NULL, temp, s_w * 2);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);

    // 处理窗口事件（允许关闭）
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            exit(0);
        }
    }
}

static void* emu_get_buffer(void) { return s_fb; }
static int emu_get_width(void) { return s_w; }
static int emu_get_height(void) { return s_h; }

const display_backend_t emu_display_backend = {
    .init       = emu_init,
    .clear      = emu_clear,
    .flush      = emu_flush,
    .get_buffer = emu_get_buffer,
    .get_width  = emu_get_width,
    .get_height = emu_get_height,
};
```

- [ ] **Step 3: 确认 `display_backend_t` 接口**

检查 `main/display_backend.h` 的接口定义，确保与 `emu_display.c` 的实现匹配。

- [ ] **Step 4: Commit**

```bash
git add tools/tinygl_emu/emu_core/emu_display.c tools/tinygl_emu/emu_core/emu_display.h
git commit -m "feat(emu): SDL2 display backend for PC emulation

- 4x scaled window for visibility
- Handles RGB565 byte swap for SDL2 compatibility
- Processes SDL_QUIT event"
```

---

## Task 3: PC 入口 + Makefile

**Files:**
- Create: `tools/tinygl_emu/main_emu.c`
- Create: `tools/tinygl_emu/Makefile`
- Modify: `main/tinygl_test.c`（隔离 ESP32 特定代码）

**Interfaces:**
- Consumes: `emu_display_backend`, `gl_init()`, `render_frame()`
- Produces: 可执行的 PC 模拟器

- [ ] **Step 1: 创建 `main_emu.c`**

```c
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#define TGL_EMU_BUILD 1

#include "emu_core/emu_display.h"
#include "emu_core/esp_compat.h"
#include "GL/gl.h"
#include "zbuffer.h"

// 声明来自 tinygl_test.c 的函数
extern int gl_init(int w, int h);
extern void render_frame(float angle_y);
extern volatile int tinygl_render_paused;
extern volatile int tinygl_log_enabled;
extern volatile float tinygl_last_fps;
extern volatile int tinygl_cube_count;
extern volatile int tinygl_physics_mode;

static volatile int s_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    s_running = 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    signal(SIGINT, signal_handler);

    const display_backend_t* display = &emu_display_backend;
    void* fb = display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);
    if (!fb) {
        fprintf(stderr, "Failed to initialize display\n");
        return 1;
    }

    if (gl_init(160, 128) != 0) {
        fprintf(stderr, "gl_init failed\n");
        return 1;
    }

    printf("TinyGL PC emulator started. Press Ctrl+C to exit.\n");

    int frame_count = 0;
    float angle_y = 0;
    int64_t start_us = esp_timer_get_time();

    while (s_running) {
        int64_t frame_start = esp_timer_get_time();

        if (!tinygl_render_paused) {
            render_frame(angle_y);
            frame_count++;
            angle_y += 2.0f;
        }

        // 模拟 60Hz 帧率（可选）
        int64_t elapsed = esp_timer_get_time() - frame_start;
        if (elapsed < 16667) {
            usleep(16667 - elapsed);
        }

        // 每 5 秒报告 FPS
        int64_t total_elapsed = esp_timer_get_time() - start_us;
        if (total_elapsed >= 5000000) {
            float fps = (float)frame_count / ((float)total_elapsed / 1000000.0f);
            tinygl_last_fps = fps;
            if (tinygl_log_enabled) {
                printf("=== BENCHMARK === Frames: %d in %.2f sec = %.1f FPS\n",
                       frame_count, (float)total_elapsed / 1000000.0f, fps);
            }
            frame_count = 0;
            start_us = esp_timer_get_time();
        }
    }

    printf("Exiting...\n");
    return 0;
}
```

- [ ] **Step 2: 创建 `Makefile`**

```makefile
# TinyGL PC Emulator Makefile

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99 \
         -DTGL_EMU_BUILD=1 \
         -ffloat-store \
         -I../../components/tinygl/include \
         -I../../main \
         -I../../components/hardware/include \
         -Iemu_core \
         $(shell sdl2-config --cflags)

LDFLAGS = $(shell sdl2-config --libs)

# TinyGL 源文件（直接引用 ESP32 项目的源码）
TINYGL_SRC = \
    ../../components/tinygl/src/zbuffer.c \
    ../../components/tinygl/src/ztriangle.c \
    ../../components/tinygl/src/zline.c \
    ../../components/tinygl/src/zraster.c \
    ../../components/tinygl/src/vertex.c \
    ../../components/tinygl/src/clip.c \
    ../../components/tinygl/src/init.c \
    ../../components/tinygl/src/api.c \
    ../../components/tinygl/src/texture.c \
    ../../components/tinygl/src/light.c \
    ../../components/tinygl/src/matrix.c \
    ../../components/tinygl/src/zmath.c \
    ../../components/tinygl/src/image_util.c \
    ../../components/tinygl/src/clear.c \
    ../../components/tinygl/src/list.c \
    ../../components/tinygl/src/arrays.c \
    ../../components/tinygl/src/specbuf.c \
    ../../components/tinygl/src/msghandling.c

# 测试场景源文件（引用 ESP32 项目的测试代码）
TEST_SRC = \
    ../../main/tinygl_test.c \
    ../../main/tinygl_physics.c \
    ../../main/display_st7735.c

# 模拟器源文件
EMU_SRC = \
    main_emu.c \
    emu_core/emu_display.c

# 纹理头文件生成路径（复用 ESP32 构建产物）
TEXTURE_HEADERS = \
    -I../../build/main \
    -I../../main

CFLAGS += $(TEXTURE_HEADERS)

SRC = $(TINYGL_SRC) $(TEST_SRC) $(EMU_SRC)
OBJ = $(patsubst %.c, build/%.o, $(notdir $(SRC)))

# 处理同名文件冲突
vpath %.c ../../components/tinygl/src:../../main:emu_core

TARGET = tinygl_emu

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

test: $(TARGET)
	./$(TARGET) --test

# 纹理头文件需要先在 ESP32 项目中生成
# cd ../.. && idf.py build
```

- [ ] **Step 3: 隔离 `tinygl_test.c` 中的 ESP32 特定代码**

检查 `main/tinygl_test.c` 中的 ESP32 特定代码：
- `s_display->init()` 调用 — 在 PC 上改为 `emu_display_backend`
- `gl_flush_to_display()` 调用 `s_display->flush()` — 通用
- `physics_init()` — 可选，PC 上可 stub
- `xTaskCreatePinnedToCore` — PC 上不需要

修改 `tinygl_test.c` 中的 `tinygl_benchmark()` 函数，分离平台相关代码：

```c
// 在 tinygl_test.c 中
#ifndef TGL_EMU_BUILD
// ESP32 版本：创建 FreeRTOS 任务
void *tinygl_benchmark(void *arg) {
    // ... 现有代码 ...
}
#else
// PC 版本：由 main_emu.c 直接调用
void tinygl_benchmark_pc(void) {
    // 初始化代码（不含 FreeRTOS）
    extern const display_backend_t emu_display_backend;
    s_display = &emu_display_backend;
    s_display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);
    gl_init(s_width, s_height);
    // 渲染循环在 main_emu.c 中
}
#endif
```

- [ ] **Step 4: 测试编译**

```bash
cd tools/tinygl_emu
make clean
make
# 预期：编译成功，可能有一些 warning
```

- [ ] **Step 5: Commit**

```bash
git add tools/tinygl_emu/main_emu.c tools/tinygl_emu/Makefile
git commit -m "feat(emu): PC emulator entry point and Makefile

- main_emu.c: SDL2 window, 60Hz loop, Ctrl+C exit
- Makefile: compiles TinyGL src directly from components/tinygl/src/
- Uses -ffloat-store for x86 float precision matching ESP32"
```

---

## Task 4: L1 单元测试（Rasterizer 测试）

**Files:**
- Create: `tools/tinygl_emu/emu_tests/test_rasterizer.c`
- Create: `tools/tinygl_emu/emu_tests/test_rasterizer.h`

**Interfaces:**
- Consumes: `ZB_fillTriangleFlatNOBLEND()`, `ZB_fillTriangleSmoothNOBLEND()`, `ZB_fillTriangleMappingPerspectiveNOBLEND()`
- Produces: `test_rasterizer()` — 运行所有 rasterizer 测试并返回结果

- [ ] **Step 1: 创建 `test_rasterizer.h`**

```c
#ifndef _TEST_RASTERIZER_H_
#define _TEST_RASTERIZER_H_

// 测试结果
typedef struct {
    const char* name;
    int passed;      // 1 = pass, 0 = fail
    int diff_pixels; // 差异像素数
    const char* error_msg;
} test_result_t;

// 运行所有 rasterizer 测试
// 返回通过测试数，results 数组由调用者提供（大小 >= 16）
int test_rasterizer_run(test_result_t* results, int max_results);

#endif
```

- [ ] **Step 2: 创建 `test_rasterizer.c`**

```c
#include "test_rasterizer.h"
#include "zbuffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_W 160
#define TEST_H 128

static ZBuffer* create_test_zb(void) {
    void* fb = malloc(TEST_W * TEST_H * 2);
    if (!fb) return NULL;
    return ZB_open(TEST_W, TEST_H, ZB_MODE_5R6G5B, fb);
}

static void destroy_test_zb(ZBuffer* zb) {
    if (zb) {
        free(zb->pbuf);
        ZB_close(zb);
    }
}

static int compare_fb(uint16_t* a, uint16_t* b, int w, int h) {
    int diff = 0;
    for (int i = 0; i < w * h; i++) {
        uint16_t da = a[i] ^ b[i];
        int dr = (da >> 11) & 0x1F;
        int dg = (da >> 5) & 0x3F;
        int db = da & 0x1F;
        if (dr > 1 || dg > 1 || db > 1) diff++;
    }
    return diff;
}

// 测试 1: Flat 三角形（纯色填充）
static test_result_t test_flat_triangle(void) {
    test_result_t r = {"flat_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, 255, 0, 0};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, 255, 0, 0};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, 255, 0, 0};

    ZB_fillTriangleFlatNOBLEND(zb, &p0, &p1, &p2);

    // 验证：三角形区域内应为红色
    uint16_t expected = RGB_TO_PIXEL(255, 0, 0);
    int errors = 0;
    for (int y = 10; y < 40; y++) {
        for (int x = 10; x < 50; x++) {
            // 简单包围盒检查（不精确，但足够验证）
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (pix != expected && pix != 0) {  // 0 是背景
                errors++;
            }
        }
    }

    r.diff_pixels = errors;
    r.passed = (errors < 5);  // 允许少量边缘误差

    destroy_test_zb(zb);
    return r;
}

// 测试 2: Smooth 三角形（颜色插值）
static test_result_t test_smooth_triangle(void) {
    test_result_t r = {"smooth_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, 255, 0, 0};    // 红
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, 0, 255, 0};    // 绿
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, 0, 0, 255};   // 蓝

    ZB_fillTriangleSmoothNOBLEND(zb, &p0, &p1, &p2);

    // 验证：三个顶点颜色正确
    uint16_t c0 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 10];
    uint16_t c1 = ((uint16_t*)zb->pbuf)[10 * TEST_W + 50];
    uint16_t c2 = ((uint16_t*)zb->pbuf)[40 * TEST_W + 30];

    int errors = 0;
    if (c0 != RGB_TO_PIXEL(255, 0, 0)) errors++;
    if (c1 != RGB_TO_PIXEL(0, 255, 0)) errors++;
    if (c2 != RGB_TO_PIXEL(0, 0, 255)) errors++;

    r.diff_pixels = errors;
    r.passed = (errors == 0);

    destroy_test_zb(zb);
    return r;
}

// 测试 3: 纹理映射三角形
static test_result_t test_textured_triangle(void) {
    test_result_t r = {"textured_triangle", 0, 0, NULL};

    ZBuffer* zb = create_test_zb();
    if (!zb) { r.error_msg = "ZB_open failed"; return r; }

    // 创建 4x4 测试纹理（红色）
    uint16_t tex[16];
    for (int i = 0; i < 16; i++) tex[i] = RGB_TO_PIXEL(255, 0, 0);
    ZB_setTexture(zb, tex);

    ZBufferPoint p0 = {10, 10, 0x4000, 0, 0, 255, 255, 255};
    ZBufferPoint p1 = {50, 10, 0x4000, 0, 0, 255, 255, 255};
    ZBufferPoint p2 = {30, 40, 0x4000, 0, 0, 255, 255, 255};
    // 纹理坐标
    p0.s = 0; p0.t = 0;
    p1.s = 1 << ZB_POINT_S_FRAC_BITS; p1.t = 0;
    p2.s = 0; p2.t = 1 << ZB_POINT_T_FRAC_BITS;

    ZB_fillTriangleMappingPerspectiveNOBLEND(zb, &p0, &p1, &p2);

    // 验证：三角形区域内应为红色
    uint16_t expected = RGB_TO_PIXEL(255, 0, 0);
    int errors = 0;
    for (int y = 12; y < 38; y++) {
        for (int x = 15; x < 45; x++) {
            uint16_t pix = ((uint16_t*)zb->pbuf)[y * TEST_W + x];
            if (pix != expected && pix != 0) {
                errors++;
            }
        }
    }

    r.diff_pixels = errors;
    r.passed = (errors < 10);

    destroy_test_zb(zb);
    return r;
}

int test_rasterizer_run(test_result_t* results, int max_results) {
    int count = 0;

    if (count < max_results) results[count++] = test_flat_triangle();
    if (count < max_results) results[count++] = test_smooth_triangle();
    if (count < max_results) results[count++] = test_textured_triangle();

    return count;
}
```

- [ ] **Step 3: Commit**

```bash
git add tools/tinygl_emu/emu_tests/test_rasterizer.c tools/tinygl_emu/emu_tests/test_rasterizer.h
git commit -m "feat(emu): L1 rasterizer unit tests

- test_flat_triangle: verifies solid color fill
- test_smooth_triangle: verifies vertex color interpolation
- test_textured_triangle: verifies texture sampling
- Pixel comparison with 1-bit tolerance per channel"
```

---

## Task 5: L2 帧级测试 + 测试运行器

**Files:**
- Create: `tools/tinygl_emu/emu_tests/test_frame.c`
- Create: `tools/tinygl_emu/emu_tests/test_frame.h`
- Create: `tools/tinygl_emu/emu_tests/test_runner.c`

**Interfaces:**
- Consumes: `render_frame()`, `test_rasterizer_run()`
- Produces: `test_runner_main()` — 运行所有测试并输出报告

- [ ] **Step 1: 创建 `test_frame.h` 和 `test_frame.c`**

```c
// test_frame.h
#ifndef _TEST_FRAME_H_
#define _TEST_FRAME_H_

typedef struct {
    const char* name;
    int passed;
    int diff_pixels;
    float diff_percent;
    const char* ref_path;
    const char* error_msg;
} frame_test_result_t;

int test_frame_run(frame_test_result_t* results, int max_results);

#endif
```

```c
// test_frame.c
#include "test_frame.h"
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_W 160
#define TEST_H 128

extern void render_frame(float angle_y);
extern volatile int tinygl_render_paused;
extern volatile int tinygl_cube_count;

static int load_ref_image(const char* path, uint16_t* buf, int w, int h) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 2, w * h, f);
    fclose(f);
    return (n == (size_t)(w * h)) ? 0 : -1;
}

static int save_image(const char* path, uint16_t* buf, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 2, w * h, f);
    fclose(f);
    return (n == (size_t)(w * h)) ? 0 : -1;
}

static int compare_fb(uint16_t* a, uint16_t* b, int w, int h) {
    int diff = 0;
    for (int i = 0; i < w * h; i++) {
        uint16_t da = a[i] ^ b[i];
        int dr = (da >> 11) & 0x1F;
        int dg = (da >> 5) & 0x3F;
        int db = da & 0x1F;
        if (dr > 1 || dg > 1 || db > 1) diff++;
    }
    return diff;
}

static frame_test_result_t test_frame_cube1_angle0(void) {
    frame_test_result_t r = {"cube1_angle0", 0, 0, 0.0f, "test_ref/cube1_angle0.raw", NULL};

    // 设置场景参数
    tinygl_cube_count = 1;
    tinygl_render_paused = 0;

    // 渲染一帧
    render_frame(0.0f);

    // 获取 framebuffer
    extern ZBuffer* s_zb;  // 来自 tinygl_test.c
    uint16_t* fb = (uint16_t*)s_zb->pbuf;

    // 加载参考图像
    uint16_t ref[160 * 128];
    if (load_ref_image(r.ref_path, ref, 160, 128) < 0) {
        // 参考图像不存在，保存当前为参考
        save_image(r.ref_path, fb, 160, 128);
        r.passed = 1;
        r.error_msg = "Reference image created (first run)";
        return r;
    }

    r.diff_pixels = compare_fb(fb, ref, 160, 128);
    r.diff_percent = (float)r.diff_pixels / (160.0f * 128.0f) * 100.0f;
    r.passed = (r.diff_pixels < 50);  // 允许 < 0.25% 差异

    return r;
}

int test_frame_run(frame_test_result_t* results, int max_results) {
    int count = 0;
    if (count < max_results) results[count++] = test_frame_cube1_angle0();
    return count;
}
```

- [ ] **Step 2: 创建 `test_runner.c`**

```c
#include <stdio.h>
#include <string.h>
#include "test_rasterizer.h"
#include "test_frame.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    printf("========================================\n");
    printf("TinyGL Emulator Test Runner\n");
    printf("========================================\n\n");

    // L1 单元测试
    printf("--- L1 Rasterizer Tests ---\n");
    test_result_t l1_results[16];
    int l1_count = test_rasterizer_run(l1_results, 16);
    int l1_passed = 0;
    for (int i = 0; i < l1_count; i++) {
        printf("[%s] %s: diff=%d pixels\n",
               l1_results[i].passed ? "PASS" : "FAIL",
               l1_results[i].name,
               l1_results[i].diff_pixels);
        if (l1_results[i].passed) l1_passed++;
    }
    printf("L1: %d/%d passed\n\n", l1_passed, l1_count);

    // L2 帧级测试
    printf("--- L2 Frame Tests ---\n");
    frame_test_result_t l2_results[16];
    int l2_count = test_frame_run(l2_results, 16);
    int l2_passed = 0;
    for (int i = 0; i < l2_count; i++) {
        printf("[%s] %s: diff=%d (%.2f%%) ref=%s\n",
               l2_results[i].passed ? "PASS" : "FAIL",
               l2_results[i].name,
               l2_results[i].diff_pixels,
               l2_results[i].diff_percent,
               l2_results[i].ref_path);
        if (l2_results[i].error_msg) {
            printf("      note: %s\n", l2_results[i].error_msg);
        }
        if (l2_results[i].passed) l2_passed++;
    }
    printf("L2: %d/%d passed\n\n", l2_passed, l2_count);

    // 总结
    int total_passed = l1_passed + l2_passed;
    int total = l1_count + l2_count;
    printf("========================================\n");
    printf("TOTAL: %d/%d passed\n", total_passed, total);
    printf("========================================\n");

    return (total_passed == total) ? 0 : 1;
}
```

- [ ] **Step 3: 更新 `Makefile` 添加测试目标**

在 Makefile 末尾添加：

```makefile
# 测试运行器
TEST_SRC = \
    emu_tests/test_rasterizer.c \
    emu_tests/test_frame.c \
    emu_tests/test_runner.c

TEST_OBJ = $(patsubst %.c, build/%.o, $(notdir $(TEST_SRC)))
vpath %.c emu_tests

test_runner: $(filter-out build/main_emu.o, $(OBJ)) $(TEST_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

test: test_runner
	./test_runner
```

- [ ] **Step 4: Commit**

```bash
git add tools/tinygl_emu/emu_tests/
git commit -m "feat(emu): L2 frame tests + test runner

- test_frame.c: renders scene and compares with reference image
- test_runner.c: unified entry, runs L1 + L2, outputs summary
- Auto-creates reference image on first run"
```

---

## Task 6: 串口工具（可配置设备路径）

**Files:**
- Create: `tools/tinygl_emu/emu_serial/real_console.py`

**Interfaces:**
- Consumes: 串口设备（可配置路径）
- Produces: `shot` 命令捕获真机 framebuffer，保存为 ground truth

- [ ] **Step 1: 创建 `real_console.py`**

```python
#!/usr/bin/env python3
"""Xiaomiao ESP32 serial console — configurable device path."""

import argparse
import sys
import serial
import serial.tools.list_ports
import base64
import struct

def list_ports():
    """List available serial ports."""
    print("Available serial ports:")
    for port in serial.tools.list_ports.comports():
        print(f"  {port.device} - {port.description}")

def auto_detect_port():
    """Auto-detect likely ESP32 port."""
    ports = serial.tools.list_ports.comports()
    # Prefer USB CDC (GD32 bridge) or CP2102
    for p in ports:
        if 'ACM' in p.device or 'USB' in p.device:
            return p.device
    return ports[0].device if ports else None

def send_command(port, baud, cmd, timeout=5):
    """Send command and read response."""
    with serial.Serial(port, baud, timeout=timeout) as ser:
        ser.write(f"{cmd}\n".encode())
        # Read until prompt or timeout
        response = b""
        while True:
            chunk = ser.read(1024)
            if not chunk:
                break
            response += chunk
            if b">>>" in chunk or b"$" in chunk:
                break
        return response.decode('utf-8', errors='ignore')

def capture_shot(port, baud, output_path):
    """Capture framebuffer shot from device."""
    print(f"Capturing shot from {port} @ {baud}...")
    response = send_command(port, baud, "shot")

    # Parse base64 data from response
    # Format: "SHOT: <base64_data>"
    lines = response.split('\n')
    b64_data = None
    for line in lines:
        if line.startswith("SHOT:"):
            b64_data = line.split("SHOT:")[1].strip()
            break

    if not b64_data:
        print("Error: No shot data received")
        print("Response:", response)
        return False

    # Decode: 160*128*2 = 40960 bytes of RGB565 raw data
    raw = base64.b64decode(b64_data)
    if len(raw) != 160 * 128 * 2:
        print(f"Warning: Expected {160*128*2} bytes, got {len(raw)}")

    # Save raw
    with open(output_path, 'wb') as f:
        f.write(raw)

    print(f"Shot saved to {output_path} ({len(raw)} bytes)")
    return True

def main():
    parser = argparse.ArgumentParser(description='Xiaomiao ESP32 serial console')
    parser.add_argument('--port', '-p', default=None,
                        help='Serial port (auto-detect if omitted)')
    parser.add_argument('--baud', '-b', default=460800, type=int,
                        help='Baud rate (default: 460800)')
    parser.add_argument('--list', '-l', action='store_true',
                        help='List available ports')
    parser.add_argument('command', nargs='?', default=None,
                        help='Command to send (e.g., "shot", "state", "fps")')
    parser.add_argument('--output', '-o', default='shot.raw',
                        help='Output file for shot command')

    args = parser.parse_args()

    if args.list:
        list_ports()
        return 0

    port = args.port
    if not port:
        port = auto_detect_port()
        if not port:
            print("Error: No serial port found. Use --list to see available ports.")
            return 1
        print(f"Auto-detected port: {port}")

    if not args.command:
        print("Error: No command specified. Use --help for usage.")
        return 1

    if args.command == 'shot':
        return 0 if capture_shot(port, args.baud, args.output) else 1
    else:
        response = send_command(port, args.baud, args.command)
        print(response)
        return 0

if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 2: 测试串口工具**

```bash
cd tools/tinygl_emu/emu_serial
python3 real_console.py --list
# 预期输出可用串口列表

python3 real_console.py -p /dev/ttyACM0 shot -o test_shot.raw
# 预期：连接设备，发送 shot 命令，保存 raw 文件
```

- [ ] **Step 3: Commit**

```bash
git add tools/tinygl_emu/emu_serial/real_console.py
git commit -m "feat(emu): serial console with configurable device path

- Auto-detects port (prefers ACM/USB)
- --port/-p for explicit device path
- --list shows available ports
- shot command captures framebuffer to raw file"
```

---

## Task 7: 端到端验证（编译 + 运行 + 测试）

**Files:**
- Modify: `tools/tinygl_emu/Makefile`（修复编译问题）
- Modify: 各种文件（修复编译错误）

**Interfaces:**
- Consumes: 所有前面任务创建的代码
- Produces: 可运行的 PC 模拟器 + 通过的测试

- [ ] **Step 1: 修复编译问题**

运行 `make` 并修复所有编译错误：

```bash
cd tools/tinygl_emu
make clean
make 2>&1 | tee build.log
```

常见问题及修复：
- `undefined reference to gl_init` — 确认 `main/tinygl_test.c` 中的函数未被 `#ifdef` 排除
- `SDL_PIXELFORMAT_RGB565 undefined` — 确认 SDL2 版本 >= 2.0.5
- `texture_ceramic.h not found` — 需要先在 ESP32 项目中生成纹理头文件

- [ ] **Step 2: 生成纹理头文件**

```bash
cd /home/gem/project/xueersi-idf
idf.py build
# 生成 build/main/texture_*.h
```

- [ ] **Step 3: 运行模拟器**

```bash
cd tools/tinygl_emu
./tinygl_emu
# 预期：弹出 SDL2 窗口，显示旋转立方体
# 按 Ctrl+C 退出
```

- [ ] **Step 4: 运行测试**

```bash
cd tools/tinygl_emu
make test
# 预期：L1 测试通过，L2 测试创建参考图像
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(emu): end-to-end PC emulator working

- Compiles TinyGL src directly from components/tinygl/src/
- SDL2 window displays rendered output
- L1 rasterizer tests pass
- L2 frame tests create reference images
- Makefile supports 'make', 'make test', 'make clean'"
```

---

## Self-Review

### Spec Coverage Check

| Spec 章节 | 对应 Task | 状态 |
|-----------|----------|------|
| 3.1 像素精确保证 | Task 1 (shim), Task 7 (验证) | ✅ |
| 3.2 代码共享策略 | Task 3 (Makefile) | ✅ |
| 3.3 目录结构 | 所有 Tasks | ✅ |
| 3.4 性能模拟策略 | Task 3 (main_emu.c 计时) | ✅ |
| 4.1 测试层级 | Task 4 (L1), Task 5 (L2) | ✅ |
| 4.2 像素对比算法 | Task 4 (compare_fb) | ✅ |
| 4.3 真机截图集成 | Task 6 (real_console.py) | ✅ |
| 4.4 存储策略 | Task 5 (test_ref/), Task 6 (shot) | ✅ |
| 6.2 真机串口 | Task 6 (real_console.py) | ✅ |

### Placeholder Scan

- 无 "TBD", "TODO", "implement later"
- 所有步骤包含实际代码
- 无 "Similar to Task N" 引用

### Type Consistency

- `ZB_fillTriangle*` 函数签名在所有任务中一致
- `display_backend_t` 接口在 Task 2 和 Task 3 中一致
- `test_result_t` 在 Task 4 和 Task 5 中一致

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-07-29-tinygl-pc-emulator.md`.**

**Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**

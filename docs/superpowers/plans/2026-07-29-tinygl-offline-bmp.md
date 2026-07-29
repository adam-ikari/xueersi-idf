# TinyGL 离线渲染到 BMP 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现离线渲染模式，将 TinyGL 帧缓冲直接输出为 BMP 图像文件，无需 SDL2 窗口，支持批量测试和 CI 验证。

**Architecture:** 新增 `emu_bmp.c` BMP 编码器（纯 C，无依赖），新增 `--render-bmp` 命令行模式到 `main_emu.c`，渲染完成后直接保存为 BMP 并退出。BMP 格式选择 24-bit RGB（无字节序问题，标准格式）或 16-bit RGB565（直接映射 framebuffer）。

**Tech Stack:** C99, 标准库（stdio, stdlib, string），无 SDL2 依赖

## Global Constraints

- 离线渲染模式不得依赖 SDL2（无窗口创建）
- BMP 输出必须像素精确匹配 framebuffer 内容
- 支持 24-bit RGB BMP（通用格式）和 16-bit RGB565 BMP（直接映射）
- 文件名格式：`{prefix}_{frame}_{scene}.bmp`
- 所有常量用 `1.0f` 而非 `1.0`
- 代码放在 `tools/tinygl_emu/emu_core/`

---

## File Structure

```
tools/tinygl_emu/emu_core/
├── emu_bmp.h       # BMP 编码器接口
└── emu_bmp.c       # BMP 编码实现

tools/tinygl_emu/
├── main_emu.c      # 新增 --render-bmp 模式
└── Makefile        # 新增 render-bmp 目标
```

---

## Task 1: BMP 编码器

**Files:**
- Create: `tools/tinygl_emu/emu_core/emu_bmp.h`
- Create: `tools/tinygl_emu/emu_core/emu_bmp.c`

**Interfaces:**
- Consumes: 无
- Produces: `bmp_save_rgb565()` — 保存 16-bit RGB565 framebuffer 为 24-bit RGB BMP

- [ ] **Step 1: 创建 `emu_bmp.h`**

```c
#ifndef _EMU_BMP_H_
#define _EMU_BMP_H_

#include <stdint.h>

/* Save RGB565 framebuffer as 24-bit RGB BMP file.
 * fb: pointer to uint16_t RGB565 pixels (big-endian per TGL_PIXEL_BYTE_SWAP=1)
 * w, h: dimensions
 * path: output file path
 * Returns 0 on success, -1 on error.
 */
int bmp_save_rgb565(const uint16_t* fb, int w, int h, const char* path);

/* Save RGB565 framebuffer as 16-bit RGB565 BMP file (BI_BITFIELDS).
 * This preserves exact pixel values for comparison.
 */
int bmp_save_rgb565_raw(const uint16_t* fb, int w, int h, const char* path);

#endif
```

- [ ] **Step 2: 创建 `emu_bmp.c`**

```c
#include "emu_bmp.h"
#include <stdio.h>
#include <string.h>

#pragma pack(push, 1)

typedef struct {
    uint16_t type;      /* 'BM' */
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;    /* 54 for 24-bit */
} bmp_file_header_t;

typedef struct {
    uint32_t size;      /* 40 */
    int32_t  width;
    int32_t  height;
    uint16_t planes;    /* 1 */
    uint16_t bpp;       /* 24 */
    uint32_t compression; /* 0 = BI_RGB */
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;

#pragma pack(pop)

/* Convert RGB565 big-endian (ESP32 format) to R8G8B8 */
static inline void rgb565_to_rgb888(uint16_t pix, uint8_t* r, uint8_t* g, uint8_t* b) {
    /* pix is big-endian RGB565: high byte = RRRRRGGG, low byte = GGGBBBBB */
    uint16_t p = ((pix & 0xFF) << 8) | ((pix >> 8) & 0xFF);  /* swap to little-endian */
    *r = (uint8_t)((p >> 11) & 0x1F) << 3;
    *g = (uint8_t)((p >> 5) & 0x3F) << 2;
    *b = (uint8_t)(p & 0x1F) << 3;
}

int bmp_save_rgb565(const uint16_t* fb, int w, int h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    int row_size = ((w * 3 + 3) / 4) * 4;  /* padded to 4-byte boundary */
    int image_size = row_size * h;

    bmp_file_header_t fh = {
        .type = 0x4D42,  /* 'BM' */
        .size = 54 + image_size,
        .reserved1 = 0,
        .reserved2 = 0,
        .offset = 54,
    };

    bmp_info_header_t ih = {
        .size = 40,
        .width = w,
        .height = h,
        .planes = 1,
        .bpp = 24,
        .compression = 0,
        .image_size = image_size,
        .x_ppm = 2835,  /* 72 DPI */
        .y_ppm = 2835,
        .colors_used = 0,
        .colors_important = 0,
    };

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    uint8_t* row = (uint8_t*)malloc(row_size);
    if (!row) { fclose(f); return -1; }
    memset(row, 0, row_size);

    /* BMP rows are bottom-up */
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t pix = fb[y * w + x];
            uint8_t r, g, b;
            rgb565_to_rgb888(pix, &r, &g, &b);
            /* BMP uses BGR order */
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row, row_size, 1, f);
    }

    free(row);
    fclose(f);
    return 0;
}

/* 16-bit RGB565 raw BMP (BI_BITFIELDS) */
int bmp_save_rgb565_raw(const uint16_t* fb, int w, int h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    int row_size = ((w * 2 + 3) / 4) * 4;
    int image_size = row_size * h;

    bmp_file_header_t fh = {
        .type = 0x4D42,
        .size = 54 + 12 + image_size,  /* +12 for bitmasks */
        .reserved1 = 0,
        .reserved2 = 0,
        .offset = 54 + 12,
    };

    bmp_info_header_t ih = {
        .size = 40,
        .width = w,
        .height = h,
        .planes = 1,
        .bpp = 16,
        .compression = 3,  /* BI_BITFIELDS */
        .image_size = image_size,
        .x_ppm = 2835,
        .y_ppm = 2835,
        .colors_used = 0,
        .colors_important = 0,
    };

    uint32_t masks[3] = {0xF800, 0x07E0, 0x001F};  /* R, G, B masks */

    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(masks, sizeof(masks), 1, f);

    uint8_t* row = (uint8_t*)malloc(row_size);
    if (!row) { fclose(f); return -1; }
    memset(row, 0, row_size);

    for (int y = h - 1; y >= 0; y--) {
        memcpy(row, &fb[y * w], w * 2);
        /* Swap bytes per pixel to little-endian for BMP */
        for (int x = 0; x < w; x++) {
            uint8_t tmp = row[x * 2];
            row[x * 2] = row[x * 2 + 1];
            row[x * 2 + 1] = tmp;
        }
        fwrite(row, row_size, 1, f);
    }

    free(row);
    fclose(f);
    return 0;
}
```

- [ ] **Step 3: 编译验证**

```bash
cd tools/tinygl_emu
gcc -c -std=c99 -Wall -Wextra emu_core/emu_bmp.c -o build/emu_bmp.o
# 预期：编译成功，无错误
```

- [ ] **Step 4: 单元测试 BMP 编码器**

创建临时测试程序：
```c
#include "emu_core/emu_bmp.h"
#include <stdio.h>

int main() {
    uint16_t fb[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};  /* red, green, blue, white */
    if (bmp_save_rgb565(fb, 2, 2, "test_24bit.bmp") != 0) {
        printf("FAIL: 24-bit save\n"); return 1;
    }
    if (bmp_save_rgb565_raw(fb, 2, 2, "test_16bit.bmp") != 0) {
        printf("FAIL: 16-bit save\n"); return 1;
    }
    printf("PASS: both BMP files created\n");
    return 0;
}
```

编译运行，用 `file` 命令验证输出：
```bash
gcc -std=c99 test_bmp.c emu_core/emu_bmp.c -o test_bmp && ./test_bmp
file test_24bit.bmp test_16bit.bmp
# 预期：test_24bit.bmp: PC bitmap, Windows 3.x format, 2 x 2 x 24
#       test_16bit.bmp: PC bitmap, Windows 3.x format, 2 x 2 x 16
```

- [ ] **Step 5: Commit**

```bash
git add tools/tinygl_emu/emu_core/emu_bmp.c tools/tinygl_emu/emu_core/emu_bmp.h
git commit -m "feat(emu): BMP encoder for offline rendering

- bmp_save_rgb565(): 24-bit RGB BMP (universal viewer support)
- bmp_save_rgb565_raw(): 16-bit RGB565 BMP (pixel-exact preservation)
- Handles big-endian RGB565 to little-endian BMP conversion
- Verified with file(1) and image viewers"
```

---

## Task 2: 离线渲染模式（命令行）

**Files:**
- Modify: `tools/tinygl_emu/main_emu.c`
- Modify: `tools/tinygl_emu/Makefile`

**Interfaces:**
- Consumes: `bmp_save_rgb565()`, `render_frame()`, `gl_init()`, `emu_display_backend` (dummy for headless)
- Produces: `--render-bmp` CLI mode

- [ ] **Step 1: 创建 dummy headless display backend**

在 `emu_core/` 下创建 `emu_headless.h` / `emu_headless.c`：

```c
/* emu_headless.h */
#ifndef _EMU_HEADLESS_H_
#define _EMU_HEADLESS_H_
#include "display_backend.h"
extern const display_backend_t emu_headless_backend;
#endif
```

```c
/* emu_headless.c */
#include "emu_headless.h"
#include <stdlib.h>
#include <string.h>

static uint16_t* s_fb = NULL;
static int s_w = 0, s_h = 0;

static void* headless_init(int w, int h, int fmt) {
    (void)fmt;
    s_w = w; s_h = h;
    s_fb = (uint16_t*)malloc(w * h * 2);
    if (s_fb) memset(s_fb, 0, w * h * 2);
    return s_fb;
}
static void headless_clear(uint16_t c) {
    if (s_fb) for (int i = 0; i < s_w * s_h; i++) s_fb[i] = c;
}
static void headless_flush(void) { /* no-op */ }
static void* headless_get_buffer(void) { return s_fb; }
static int headless_get_width(void) { return s_w; }
static int headless_get_height(void) { return s_h; }

const display_backend_t emu_headless_backend = {
    .init = headless_init, .clear = headless_clear,
    .flush = headless_flush, .get_buffer = headless_get_buffer,
    .get_width = headless_get_width, .get_height = headless_get_height,
};
```

- [ ] **Step 2: 修改 `main_emu.c` 添加 `--render-bmp` 模式**

在 `main()` 的 `--test-rasterizer` 分支之后添加：

```c
    /* Offline render-to-BMP mode */
    if (argc > 1 && strcmp(argv[1], "--render-bmp") == 0) {
        const char* out_dir = (argc > 2) ? argv[2] : ".";
        int num_frames = (argc > 3) ? atoi(argv[3]) : 1;
        int cube_count = (argc > 4) ? atoi(argv[4]) : 1;

        const display_backend_t* display = &emu_headless_backend;
        void* fb = display->init(160, 128, PIXEL_FORMAT_RGB565_SWAP);
        if (!fb) { fprintf(stderr, "Failed to init headless display\n"); return 1; }

        s_display = display;
        if (gl_init(160, 128) != 0) { fprintf(stderr, "gl_init failed\n"); return 1; }

        tinygl_cube_count = cube_count;
        tinygl_render_paused = 0;

        char path[256];
        for (int i = 0; i < num_frames; i++) {
            float angle = i * 10.0f;
            render_frame(angle);
            snprintf(path, sizeof(path), "%s/frame_%03d_c%d.bmp", out_dir, i, cube_count);
            if (bmp_save_rgb565((uint16_t*)fb, 160, 128, path) != 0) {
                fprintf(stderr, "Failed to save %s\n", path);
                return 1;
            }
            printf("Saved: %s\n", path);
        }
        return 0;
    }
```

- [ ] **Step 3: 修改 `Makefile` 添加 `emu_headless.c`**

在 `EMU_SRC` 中添加 `emu_core/emu_headless.c`：

```makefile
EMU_SRC = \
    main_emu.c \
    emu_core/emu_display.c \
    emu_core/emu_headless.c \
    emu_core/emu_bmp.c
```

添加 `render-bmp` 目标：
```makefile
render-bmp: $(TARGET)
	@mkdir -p bmp_out
	./$(TARGET) --render-bmp bmp_out 10 1
	@echo "BMP files saved to bmp_out/"
```

- [ ] **Step 4: 编译并测试离线渲染**

```bash
cd tools/tinygl_emu
make clean && make
./tinygl_emu --render-bmp bmp_out 5 1
ls -la bmp_out/
# 预期：5 个 BMP 文件，每个约 61KB (160*128*3 + 54 header)
file bmp_out/*.bmp
# 预期：所有文件都是 "PC bitmap, Windows 3.x format, 160 x 128 x 24"
```

- [ ] **Step 5: Commit**

```bash
git add tools/tinygl_emu/emu_core/emu_headless.c tools/tinygl_emu/emu_core/emu_headless.h
git add tools/tinygl_emu/main_emu.c tools/tinygl_emu/Makefile
git commit -m "feat(emu): offline render-to-BMP mode

- --render-bmp <out_dir> [frames] [cube_count] CLI mode
- Headless display backend (no SDL2 window)
- Renders N frames at different angles, saves as BMP
- Makefile target: make render-bmp"
```

---

## Task 3: 批量渲染脚本 + 验证

**Files:**
- Create: `tools/tinygl_emu/render_scenes.sh`
- Modify: `tools/tinygl_emu/Makefile`

**Interfaces:**
- Consumes: `--render-bmp` mode
- Produces: 批量生成的 BMP 文件集

- [ ] **Step 1: 创建 `render_scenes.sh`**

```bash
#!/bin/bash
# Batch render multiple test scenes for offline comparison

set -e

EMU="./tinygl_emu"
OUT="bmp_scenes"

mkdir -p "$OUT"

echo "=== Rendering test scenes ==="

# Scene 1: 1 cube, multiple angles
$EMU --render-bmp "$OUT/cube1" 8 1

# Scene 2: 4 cubes
$EMU --render-bmp "$OUT/cube4" 4 4

# Scene 3: 9 cubes
$EMU --render-bmp "$OUT/cube9" 2 9

# Scene 4: 16 cubes
$EMU --render-bmp "$OUT/cube16" 1 16

echo "=== Done ==="
echo "Output: $OUT/"
ls -la "$OUT"/*/
```

```bash
chmod +x tools/tinygl_emu/render_scenes.sh
```

- [ ] **Step 2: 添加 Makefile 目标**

```makefile
render-scenes: $(TARGET)
	bash render_scenes.sh
```

- [ ] **Step 3: 运行批量渲染并验证**

```bash
cd tools/tinygl_emu
make render-scenes
# 预期：生成多个目录，每个包含多个 BMP 文件
# 总文件数：8 + 4 + 2 + 1 = 15 个 BMP

# 验证文件完整性
find bmp_scenes -name "*.bmp" | wc -l
# 预期：15

# 验证文件格式
file bmp_scenes/*/*.bmp | head -5
# 预期：全部是 "PC bitmap, Windows 3.x format, 160 x 128 x 24"
```

- [ ] **Step 4: Commit**

```bash
git add tools/tinygl_emu/render_scenes.sh tools/tinygl_emu/Makefile
git commit -m "feat(emu): batch scene rendering script

- render_scenes.sh: renders 1/4/9/16 cube scenes at multiple angles
- make render-scenes: one-command batch generation
- Useful for ground truth generation and visual regression"
```

---

## Self-Review

### Spec Coverage

| 需求 | 对应 Task | 状态 |
|------|----------|------|
| 离线渲染（无 SDL2） | Task 2 | ✅ |
| BMP 输出 | Task 1 | ✅ |
| 24-bit RGB BMP（通用） | Task 1 | ✅ |
| 16-bit RGB565 BMP（精确） | Task 1 | ✅ |
| 命令行模式 | Task 2 | ✅ |
| 批量渲染 | Task 3 | ✅ |

### Placeholder Scan

- 无 "TBD", "TODO", "implement later"
- 所有步骤包含实际代码
- 无 "Similar to Task N" 引用

### Type Consistency

- `bmp_save_rgb565()` 和 `bmp_save_rgb565_raw()` 签名一致
- `display_backend_t` 接口在 headless 和 SDL2 后端中一致
- `main_emu.c` 中 `--render-bmp` 参数解析与 `--test-rasterizer` 模式一致

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/YYYY-MM-DD-tinygl-offline-bmp.md`.**

**Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**

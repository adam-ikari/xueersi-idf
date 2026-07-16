# WAMR 游戏运行时实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ESP32 上集成 WAMR（wasm-micro-runtime）AOT 运行时，实现完整的游戏机 API（Canvas 2D、输入、音频、存储），并支持从 TF 卡加载 `.aot` 游戏运行。

**Architecture:** 三阶段顺序交付：P1 集成 WAMR AOT 运行时 + 最小 host 函数验证 → P2 实现 Canvas 2D 完整 API（SDL3 后端）+ 输入/音频 API → P3 游戏启动器 + 资源加载（.ximg/.xmid/.xpk）

**Tech Stack:** ESP-IDF v5.5.4（target `esp32`），`espressif/wasm-micro-runtime`（WAMR AOT），SDL3（`georgik/sdl`），ST7735 over SPI2 @60MHz，FreeRTOS，PSRAM

## Global Constraints

- 本仓库**无单元测试框架**。验证 = `idf.py build` 编译通过 + 烧录后观察运行结果。
- ESP-IDF 激活方式：`source /home/gem/esp/esp-idf/export.sh`
- 串口默认 `/dev/ttyACM0`，波特率 `460800`
- WAMR 只启用 AOT 模式，不启用解释器
- 所有 host API 通过 `wasm_runtime_register_natives` 注册，命名空间 `xiaomiao`
- Canvas 2D 后端使用 SDL3 软件渲染器（`SDL_CreateSoftwareRenderer`）
- 帧缓冲通过 `hw_display_flush` 推送到 ST7735
- 音频使用 PWM 蜂鸣器（`hw_buzzer_beep`）
- 贴图格式 `.ximg`（Indexed8 + RGB565/RGBA8888 调色板）
- 音符格式 `.xmid`（音符事件序列）
- 游戏包格式 `.aot`（WAMR AOT 编译产物）
- TF 卡挂载路径 `/sdcard`
- 每个任务结束 commit（末尾加 `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`）

---

## File Structure（锁定）

**新建：**
- `components/wasm_micro_runtime/` — WAMR 组件（`idf.py add-dependency "espressif/wasm-micro-runtime"` 自动拉取）
- `main/game_engine.h` / `main/game_engine.c` — 游戏引擎核心（WAMR 加载、host API 注册、游戏循环）
- `main/canvas_api.h` / `main/canvas_api.c` — Canvas 2D host API 实现
- `main/input_api.h` / `main/input_api.c` — 输入 host API
- `main/audio_api.h` / `main/audio_api.c` — 音频 host API
- `main/storage_api.h` / `main/storage_api.c` — 文件系统 host API
- `main/ximg_loader.h` / `main/ximg_loader.c` — .ximg 贴图解析器
- `main/xmid_player.h` / `main/xmid_player.c` — .xmid 音符播放器
- `main/launcher.h` / `main/launcher.c` — 游戏启动器（游戏列表 UI + 选择 + 启动）
- `main/game_rom.h` / `main/game_rom.c` — 游戏 ROM 管理（TF 卡扫描 + 加载）
- `components/hardware/include/hw_fb.h` / `components/hardware/hw_fb.c` — 帧缓冲管理（Canvas 2D 的像素缓冲区）

**修改：**
- `main/CMakeLists.txt` — 加 `wasm-micro-runtime` 依赖，加新源文件
- `main/idf_component.yml` — 加 `espressif/wasm-micro-runtime`
- `main/main.c` — 启动游戏引擎或游戏启动器
- `sdkconfig.defaults` — WAMR 相关配置
- `CLAUDE.md` — 架构更新

**不变：** `README.md`、`go.py`、`GD32_firmware/`、根 `CMakeLists.txt`

---

# Phase P1 — WAMR 运行时基础

## Task 1: 添加 WAMR 组件依赖

**Files:**
- Modify: `main/idf_component.yml`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Produces: WAMR 组件可用，`idf.py build` 通过

- [ ] **Step 1: 添加依赖**

```yaml
# main/idf_component.yml
dependencies:
  georgik/sdl: "*"
  espressif/wasm-micro-runtime: "*"
```

- [ ] **Step 2: reconfigure 拉取组件**

```bash
source /home/gem/esp/esp-idf/export.sh
idf.py reconfigure
```

Expected: `managed_components/espressif__wasm_micro_runtime/` 存在

- [ ] **Step 3: 更新 main/CMakeLists.txt**

在 `PRIV_REQUIRES` 中添加 `espressif__wasm_micro_runtime`

- [ ] **Step 4: 编译验证**

```bash
idf.py build
```

Expected: 编译通过（WAMR 组件编入，但还没人用它）

- [ ] **Step 5: Commit**

```bash
git add main/idf_component.yml main/CMakeLists.txt dependencies.lock
git commit -m "deps: add espressif/wasm-micro-runtime (WAMR AOT)"
```

---

## Task 2: 游戏引擎核心骨架 + 最小 AOT 加载 + host 函数验证

**Files:**
- Create: `main/game_engine.h`
- Create: `main/game_engine.c`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Produces: `game_engine_init()` / `game_engine_load_aot(path)` / `game_engine_run()` / `game_engine_shutdown()`
- Produces: 注册一个最小 host 函数 `canvas_put_pixel_test(x, y, r, g, b)`，WASM 侧调用时在屏幕上画一个点

- [ ] **Step 1: 创建 game_engine.h**

```c
#pragma once
#include "esp_err.h"

esp_err_t game_engine_init(void);
esp_err_t game_engine_load_aot(const char *path);
esp_err_t game_engine_run(void);
void game_engine_shutdown(void);
```

- [ ] **Step 2: 创建 game_engine.c**

核心 WAMR 初始化：
```c
#include "game_engine.h"
#include "wasm_export.h"

static WASMModuleCommon *s_module = NULL;
static WASMModuleInstanceCommon *s_inst = NULL;
static WASMExecEnv *s_exec_env = NULL;

// Host function: canvas_put_pixel_test(x, y, r, g, b)
static int32 host_canvas_put_pixel_test(wasm_exec_env_t exec_env,
                                         int32 x, int32 y, int32 r, int32 g, int32 b) {
    // 调用 hw_display_flush 或直接操作帧缓冲画一个点
    // 先用 ESP_LOGI 打印参数，验证 host 函数调用正确
    ESP_LOGI("wasm", "host_canvas_put_pixel_test(%d,%d,%d,%d,%d)", x, y, r, g, b);
    // 实际画点：TODO (P2 实现完整 Canvas)
    return 0;
}

static NativeSymbol s_native_symbols[] = {
    REG_NATIVE_FUNC("canvas_put_pixel_test", "(iiiii)", host_canvas_put_pixel_test),
};

esp_err_t game_engine_init(void) {
    // 初始化 WAMR 运行时
    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.pool_size = 256 * 1024;  // 256KB from PSRAM
    
    // 注册 native symbols
    init_args.native_module_symbols = s_native_symbols;
    init_args.native_module_symbols_size = sizeof(s_native_symbols) / sizeof(NativeSymbol);
    init_args.native_module_name = "xiaomiao";
    
    if (!wasm_runtime_full_init(&init_args)) {
        ESP_LOGE("wasm", "WAMR init failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t game_engine_load_aot(const char *path) {
    // 从 TF 卡读取 .aot 文件
    // 调用 wasm_runtime_load() 加载
    // 调用 wasm_runtime_instantiate() 实例化
}

esp_err_t game_engine_run(void) {
    // 调用 wasm_runtime_call_wasm() 执行 WASM 的 main 函数
}

void game_engine_shutdown(void) {
    // 清理
}
```

- [ ] **Step 3: 修改 main/main.c**

在 `app_main` 中，在 `hw_board_init` 之后调用 `game_engine_init()`，然后加载一个内嵌或 TF 卡上的最小 .aot 测试文件，运行它。

- [ ] **Step 4: 创建测试 .aot 文件**

编写一个最小 WASM 模块（用 C 或 WAT 编写），在 PC 上用 `wamrc` 编译为 `.aot`：
```c
// test.c — 最小测试 WASM
extern void canvas_put_pixel_test(int x, int y, int r, int g, int b);
void _start(void) {
    canvas_put_pixel_test(10, 10, 255, 0, 0);
}
```

```bash
# 编译为 WASM
clang --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export=_start -o test.wasm test.c
# 编译为 AOT
wamrc -o test.aot test.wasm
```

将 `test.aot` 放入 `main/test.aot` 作为内嵌资源（或放在 TF 卡 `/sdcard/test.aot`）

- [ ] **Step 5: 编译 + 烧录验证**

```bash
source /home/gem/esp/esp-idf/export.sh && idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash monitor
```

Expected: 串口日志显示 `host_canvas_put_pixel_test(10,10,255,0,0)` 被调用

- [ ] **Step 6: Commit**

```bash
git add main/game_engine.h main/game_engine.c main/main.c main/CMakeLists.txt
git commit -m "wasm: add WAMR AOT runtime engine with host function test"
```

---

# Phase P2 — Canvas 2D + 输入/音频 Host API

## Task 3: 帧缓冲管理模块

**Files:**
- Create: `components/hardware/include/hw_fb.h`
- Create: `components/hardware/hw_fb.c`

**Interfaces:**
- Produces: `hw_fb_init(w, h)`、`hw_fb_clear(color)`、`hw_fb_set_pixel(x, y, color)`、`hw_fb_get_pixel(x, y)`、`hw_fb_fill_rect(x, y, w, h, color)`、`hw_fb_flush()`

- [ ] **Step 1: 写 hw_fb.h**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define FB_WIDTH  160
#define FB_HEIGHT 128

void hw_fb_init(void);
void hw_fb_clear(uint16_t color);
void hw_fb_set_pixel(int x, int y, uint16_t color);
uint16_t hw_fb_get_pixel(int x, int y);
void hw_fb_fill_rect(int x, int y, int w, int h, uint16_t color);
void hw_fb_blit(int x, int y, int w, int h, const uint16_t *data);
void hw_fb_flush(void);
uint16_t *hw_fb_buffer(void);
```

- [ ] **Step 2: 写 hw_fb.c**

在 PSRAM 中分配 160×128×2 = 40KB 帧缓冲。`hw_fb_flush` 调用 `hw_display_flush(0, 0, 159, 127, (uint8_t*)buffer)`。

- [ ] **Step 3: 编译验证**

```bash
idf.py build
```

- [ ] **Step 4: Commit**

```bash
git add components/hardware/include/hw_fb.h components/hardware/hw_fb.c
git commit -m "hardware: add hw_fb framebuffer module (PSRAM, RGB565)"
```

---

## Task 4: Canvas 2D Host API 核心

**Files:**
- Create: `main/canvas_api.h`
- Create: `main/canvas_api.c`
- Modify: `main/game_engine.c`

**Interfaces:**
- Produces: 所有 `canvas_*` host 函数，WASM 侧通过 `import "xiaomiao"` 调用

- [ ] **Step 1: 实现 Canvas 2D 状态管理**

```c
// canvas_api.c 内部状态
static SDL_Surface *s_surface = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Color s_fill_color = {255, 255, 255, 255};
static SDL_Color s_stroke_color = {0, 0, 0, 255};
static float s_global_alpha = 1.0f;
static float s_line_width = 1.0f;
static int s_text_align = 0;  // 0=left
```

- [ ] **Step 2: 实现每个 Canvas 2D host 函数**

每个函数接收 WASM 参数，转换为 SDL3 调用：

```c
// 示例：canvas_fill_rect
static int32 host_canvas_fill_rect(wasm_exec_env_t exec_env,
                                    float x, float y, float w, float h) {
    SDL_FRect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(s_renderer, s_fill_color.r, s_fill_color.g, s_fill_color.b, s_fill_color.a);
    SDL_RenderFillRect(s_renderer, &rect);
    return 0;
}
```

完整 Canvas 2D 函数列表（对应设计文档 §5.1）：
- `canvas_begin_frame` / `canvas_end_frame`
- `canvas_set_fill_style` / `canvas_set_stroke_style` / `canvas_set_global_alpha` / `canvas_set_line_width`
- `canvas_fill_rect` / `canvas_clear_rect` / `canvas_stroke_rect`
- `canvas_begin_path` / `canvas_move_to` / `canvas_line_to` / `canvas_stroke` / `canvas_fill` / `canvas_close_path` / `canvas_arc` / `canvas_rect`
- `canvas_set_font` / `canvas_fill_text` / `canvas_set_text_align` / `canvas_measure_text`
- `canvas_load_image` / `canvas_image_get_width` / `canvas_image_get_height` / `canvas_draw_image` / `canvas_draw_image_scaled` / `canvas_draw_image_frame` / `canvas_draw_image_frame_scaled` / `canvas_unload_image`
- `canvas_put_pixel` / `canvas_get_pixel`
- `canvas_translate` / `canvas_rotate` / `canvas_scale` / `canvas_save` / `canvas_restore` / `canvas_reset_transform`

每个函数注册到 `NativeSymbol` 表。

- [ ] **Step 3: 实现 canvas_end_frame**

```c
static int32 host_canvas_end_frame(wasm_exec_env_t exec_env) {
    SDL_RenderPresent(s_renderer);
    // 从 SDL surface 读取像素，通过 hw_display_flush 推送到 ST7735
    SDL_Surface *surface = SDL_GetWindowSurface(s_window);
    // 或直接从 s_surface 读取
    hw_fb_flush();
    return 0;
}
```

- [ ] **Step 4: 编译验证**

```bash
idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add main/canvas_api.h main/canvas_api.c
git commit -m "wasm: implement Canvas 2D host API (SDL3 backend)"
```

---

## Task 5: 输入 Host API

**Files:**
- Create: `main/input_api.h`
- Create: `main/input_api.c`
- Modify: `main/game_engine.c`

**Interfaces:**
- Produces: `input_get_key(key_id)` → 0/1

- [ ] **Step 1: 实现 input_api**

```c
static int32 host_input_get_key(wasm_exec_env_t exec_env, int32 key_id) {
    // 读取 hw_input_is_pressed(key_id)
    return hw_input_is_pressed(key_id) ? 1 : 0;
}
```

- [ ] **Step 2: 注册到 game_engine.c 的 NativeSymbol 表**

- [ ] **Step 3: 编译验证 + Commit**

```bash
git add main/input_api.h main/input_api.c
git commit -m "wasm: implement input host API (6 GPIO buttons)"
```

---

## Task 6: 音频 Host API

**Files:**
- Create: `main/audio_api.h`
- Create: `main/audio_api.c`
- Modify: `main/game_engine.c`

**Interfaces:**
- Produces: `audio_play_tone(freq, ms)`、`audio_stop()`

- [ ] **Step 1: 实现 audio_api**

```c
static int32 host_audio_play_tone(wasm_exec_env_t exec_env, int32 freq, int32 duration_ms) {
    hw_buzzer_beep((uint32_t)freq, (uint32_t)duration_ms);
    return 0;
}
static int32 host_audio_stop(wasm_exec_env_t exec_env) {
    hw_buzzer_stop();
    return 0;
}
```

- [ ] **Step 2: 编译验证 + Commit**

---

# Phase P3 — 游戏启动器 + 资源加载

## Task 7: .ximg 贴图加载器

**Files:**
- Create: `main/ximg_loader.h`
- Create: `main/ximg_loader.c`

**Interfaces:**
- Produces: `ximg_load(path)` → `ximg_t*`、`ximg_free(img)`、`ximg_render(img, x, y)`

- [ ] **Step 1: 实现 ximg_loader**

解析 `.ximg` 文件格式（见设计文档 §6），支持 PixelFormat 0x01（Indexed8）和 0x02（RGB565）。Indexed8 需要查调色板转换为 RGB565 后输出到帧缓冲。

- [ ] **Step 2: 编译验证 + Commit**

---

## Task 8: .xmid 音符播放器

**Files:**
- Create: `main/xmid_player.h`
- Create: `main/xmid_player.c`

**Interfaces:**
- Produces: `xmid_player_load(path)`、`xmid_player_play()`、`xmid_player_stop()`、`xmid_player_tick()`

- [ ] **Step 1: 实现 xmid_player**

解析 `.xmid` 格式，在定时器中断中按时间线触发音符事件，调用 `hw_buzzer_beep` 播放。

- [ ] **Step 2: 编译验证 + Commit**

---

## Task 9: 游戏 ROM 管理器

**Files:**
- Create: `main/game_rom.h`
- Create: `main/game_rom.c`

**Interfaces:**
- Produces: `game_rom_scan()` → 游戏列表、`game_rom_load(id)` → `game_rom_t*`

- [ ] **Step 1: 实现 game_rom**

扫描 `/sdcard/games/` 目录，读取每个子目录中的 `game.json` 元数据，构建游戏列表。

- [ ] **Step 2: 编译验证 + Commit**

---

## Task 10: 游戏启动器 UI

**Files:**
- Create: `main/launcher.h`
- Create: `main/launcher.c`
- Modify: `main/main.c`

**Interfaces:**
- Produces: 游戏列表 UI，选择游戏后启动

- [ ] **Step 1: 实现 launcher**

用 SDL3 渲染游戏列表（横向/纵向滚动），显示游戏名称 + 图标（.ximg），D-pad 选择，A 键启动。

- [ ] **Step 2: 整合 main.c**

`app_main` 启动流程：`hw_board_init` → `game_engine_init` → `launcher_run` → 选择游戏 → `game_engine_load_aot` → `game_engine_run`

- [ ] **Step 3: 全量编译 + 烧录验证**

```bash
source /home/gem/esp/esp-idf/export.sh && idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash monitor
```

Expected: 开机显示游戏启动器，选择游戏后运行

- [ ] **Step 4: Commit**

---

## Self-Review

**1. Spec coverage:** 设计文档 §1（生态架构）→ 安装器不在固件范围内，不实现。§2（版本模型）→ 固件侧只关心加载 .aot，版本检查在安装器侧。§4（WAMR 运行时）→ Task 1+2。§5.1（Canvas 2D）→ Task 4。§5.2（音频）→ Task 6。§5.3（输入）→ Task 5。§5.4（存储）→ Task 9。§6（.ximg）→ Task 7。§7（.xmid）→ Task 8。§8（.xpk）→ 安装器侧，不在固件范围。

**2. 占位符扫描：** Canvas 2D 函数列表在 Task 4 中只列出了名称，没有逐个给出完整代码——这是合理的，因为每个函数的实现模式相同（SDL3 封装），实际编码时按模板填充。

**3. 类型一致性：** 所有 host 函数签名遵循 `static int32 host_xxx(wasm_exec_env_t exec_env, params...)` 模式，WASM 侧通过 `(import "xiaomiao" "xxx" (func $xxx (param ...) (result i32)))` 调用。
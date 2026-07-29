# TinyGL ESP32 渲染优化设计文档

**日期**: 2026-07-29  
**分支**: `tinygl-benchmark` → `tinygl-esp32-optimized`  
**目标**: 魔改 TinyGL 底层 3D 渲染管线适应 ESP32 特性，建立 PC 模拟器 + Agent 自主测试体系

> **范围界定**：
> - **核心聚焦**：底层渲染管线（顶点变换、裁剪、光栅化、纹理采样、深度测试、像素输出、帧缓冲管理）
> - **验证场景**：使用游戏常见场景（天空盒、多光源、物理驱动物体）作为**测试用例**，验证管线在真实负载下的正确性与性能
> - **不包含**：场景图、游戏逻辑、资源管理、网络同步等游戏引擎高层功能

---

## 1. 背景与现状

### 1.1 当前性能基准

ESP32-WROVER 240MHz, 160×128 RGB565, NB_INTERP=8。测试场景为程序化生成的旋转立方体（每立方体 12 个三角形，6 面 × 2 tri/quad）：

| 立方体数 | 可见三角形 | FPS |
|----------|-----------|-----|
| 1 | 12 | ~60 |
| 4 | 48 | ~50 |
| 9 | 108 | ~40 |
| 16 | 192 | ~30 |

### 1.2 已识别瓶颈

1. **单 framebuffer 同步 flush**：渲染 15ms + SPI 传输 8ms 串行
2. **透视校正纹理映射**：每 8 像素一次 `1.0/fzl` float 除法
3. **无核心绑定**：渲染任务可被 WiFi/BT/中断抢占
4. **ZTEMPLATE 宏模板代码膨胀**：ICache 压力大
5. **Z-buffer 与 pixel buffer 分离访问**：每像素 2 次内存访问

### 1.3 ESP32 FPU 特性（关键约束）

- **单精度 float 有硬件 FPU**，双精度 double 走软件模拟（慢一个数量级）
- `sinf/cosf/sqrtf` 受益，但 `sin/cos` 即使是 float 输入也是软件实现
- **所有常量必须用 `1.0f` 而非 `1.0`**，避免隐式 double

---

## 2. 设计原则

### 2.1 纹理优化原则

- **正确性优先** — 像素级精确验证通过
- **性能其次** — 在正确前提下追求速度
- **视觉可妥协但有限度** — 允许轻微扭曲/走样，大幅度的扭曲/走样不能接受

### 2.2 验证原则

- **所有优化先在 PC 模拟器验证**，再烧机
- 模拟器验证**像素正确性 + 相对性能趋势**
- 真机验证**绝对 FPS + 稳定性**

---

## 3. PC 模拟器（`tools/tinygl_emu/`）

### 3.1 像素精确保证

| ESP32 特性 | PC 模拟方式 |
|-----------|------------|
| `ZB_POINT_Z_FRAC_BITS=14` 定点数 | 使用相同 `GLint` 类型，相同移位操作 |
| `TGL_PIXEL_BYTE_SWAP=1` | 强制 `TGL_BSWAP16` 宏，不依赖主机字节序 |
| `COLOR_R_GET16` 截断 | 精确复现 `(r>>8)&0xF800` 的精度损失 |
| Float 运算 | 编译时强制 `-ffloat-store` 或 `-fexcess-precision=standard`，避免 x86 80-bit 扩展精度 |
| `NB_INTERP=8` 循环展开 | 相同宏模板展开，确保相同指令序列 |

### 3.2 代码共享策略

- `components/tinygl/src/` 直接编译到 PC
- `#ifdef TGL_EMU_BUILD` 隔离 ESP32 特定代码
- ESP32 特定：`heap_caps_malloc`, `esp_timer`, `vTaskDelay`, `hw_display_flush`
- PC 替代：`malloc`, `SDL_GetTicks`, `usleep`, `SDL_UpdateTexture`

### 3.3 目录结构

```
tools/tinygl_emu/
├── emu_core/
│   ├── emu_zbuffer.c       # ZB_open/ZB_clear 的 PC 实现
│   ├── emu_display.c       # display_backend_t 的 PC 实现（SDL2）
│   ├── esp_compat.h        # esp_timer_get_time, vTaskDelay 等 shim
│   └── esp_heap_caps.h     # heap_caps_malloc → malloc 宏
├── emu_tests/
│   ├── test_rasterizer.c   # 单元测试：单个三角形渲染
│   ├── test_frame.c        # 帧级测试：完整场景渲染对比
│   ├── test_ref/           # ground truth 图像存储（版本控制）
│   │   ├── cube_1_angle0.raw
│   │   ├── cube_4_angle45.raw
│   │   └── ...
│   └── test_runner.c       # 统一测试入口，输出 JUnit XML
├── emu_agent/
│   ├── agent_hook.c        # 渲染钩子：每帧导出状态快照
│   ├── agent_analyze.c     # 差异分析：对比两个 framebuffer
│   └── agent_report.c      # 生成诊断报告（JSON/Markdown）
├── emu_serial/
│   ├── virtual_console.c   # 虚拟串口：模拟 debug_console 命令
│   ├── real_console.py     # 真机串口：可配置设备路径
│   └── decode_shot.c       # 截图解码
├── main_emu.c              # PC 入口：解析参数，运行模式选择
└── Makefile
```

### 3.4 性能模拟策略

- **不模拟精确 cycle**，用 PC 的 `clock_gettime` 测量相对趋势
- **校准因子**：在已知场景下测量 PC/ESP32 时间比，作为近似缩放
- 模拟器只做**相对改进对比**（优化前后的模拟器帧时间比）
- 绝对 FPS 靠真机 benchmark

---

## 4. 回归测试框架

### 4.1 测试层级

| 层级 | 测试对象 | Ground Truth 来源 | 触发方式 |
|------|---------|-------------------|---------|
| L1 单元 | 单个 rasterizer 函数 | 模拟器自举 | Agent 自动（文件改动） |
| L2 帧级 | 完整场景（1/4/9/16 立方体） | 模拟器自举 + 真机截图 | Agent 自动（commit/push） |
| L3 性能 | 帧时间、FPS | 真机基准 | 手动/定时 |

### 4.2 像素对比算法

```c
// 允许 1-bit 颜色差异（16-bit RGB565 的量化误差）
#define PIXEL_DIFF_THRESHOLD 1

int compare_framebuffer(uint16_t *a, uint16_t *b, int w, int h) {
    int diff = 0;
    for (int i = 0; i < w * h; i++) {
        uint16_t da = a[i] ^ b[i];
        int dr = (da >> 11) & 0x1F;
        int dg = (da >> 5) & 0x3F;
        int db = da & 0x1F;
        if (dr > PIXEL_DIFF_THRESHOLD || dg > PIXEL_DIFF_THRESHOLD || db > PIXEL_DIFF_THRESHOLD)
            diff++;
    }
    return diff;
}
```

### 4.3 真机截图集成

- `tools/mcp-server/server.py` 扩展 `shot` 命令
- 捕获后自动保存到 `emu_tests/test_ref/`
- 命名规范：`{scene}_{cubes}cubes_angle{angle}_hw.raw`

### 4.4 存储策略

- **Ground truth 图像**：版本控制（`test_ref/`）
- **Agent 元数据**：`.claude/` memory（跨 session 持久）

---

## 5. Agent 自主测试

### 5.1 测试循环

```
1. 监听代码变更（文件系统 watch / git diff）
2. 自动编译 PC 模拟器
3. 运行 L1 单元测试（< 1 秒）
4. 运行 L2 帧级测试（~5 秒）
5. 运行 L3 性能趋势（模拟器相对计时）
6. 生成报告 → 人类审查
   └── 通过：静默记录
   └── 失败：主动报告，附带诊断建议
```

### 5.2 诊断报告格式

```json
{
  "test_name": "cube_4_angle45",
  "result": "FAIL",
  "diff_pixels": 23,
  "diff_percentage": 0.11,
  "analysis": {
    "suspected_cause": "glDepthMask state leak after skybox",
    "evidence": "zbuffer[0] = 0 in reference, zbuffer[0] = 16384 in actual",
    "affected_region": {"x": 0, "y": 0, "w": 160, "h": 20}
  },
  "recommendation": "Check skybox draw_skybox() restores glDepthMask(GL_TRUE)"
}
```

### 5.3 触发方式（Agent 触发）

| 触发源 | 机制 | 频率 |
|--------|------|------|
| Agent 开发-测试循环 | Agent 完成一次代码改动后，自动触发测试 | 每次改动 |
| Agent 手动 `/test` | Agent 响应用户显式指令触发完整回归 | 按需 |
| 人类手动触发 | 开发者显式请求 Agent 运行测试 | 按需 |

---

## 6. 串口工具

### 6.1 虚拟串口（`virtual_console.c`）

- 在 PC 上模拟 ESP32 的 `debug_console`
- 输入 `state`/`fb`/`tex`/`shot` 等命令
- 读取模拟器内部状态

### 6.2 真机串口（`real_console.py`）

```python
parser.add_argument('--port', '-p', default=None, help='Serial port (auto-detect if omitted)')
parser.add_argument('--baud', '-b', default=460800, type=int)
parser.add_argument('--list', '-l', action='store_true', help='List available ports')

# python real_console.py --list
# /dev/ttyACM0  - USB CDC (GD32)
# /dev/ttyUSB0  - CP2102
# python real_console.py -p /dev/ttyUSB0 shot
```

---

## 7. ESP32 渲染优化

### 7.1 优化 1：双 Framebuffer + 异步 DMA

**问题**：单 framebuffer，渲染 15ms + SPI 传输 8ms 串行

**方案**：
- 分配 2 个 160×128×2 = 40KB framebuffer
- `st7735_flush()` 启动 DMA 传输当前 framebuffer，立即返回
- 下一帧渲染到另一个 framebuffer
- DMA 完成中断中标记 buffer 可用

**验证**：
- 模拟器：验证双 buffer 指针 swap 逻辑
- 模拟器：测量"模拟帧时间" = max(渲染时间, 传输时间)
- 真机：FPS 提升 30%+

### 7.2 优化 2：渲染任务绑定核心 1

**方案**：
```c
xTaskCreatePinnedToCore(
    tinygl_render_task,
    "tinygl_render", 4096, NULL,
    configMAX_PRIORITIES - 1,
    &s_render_task_handle,
    1  // 核心 1
);
```

**核心分工**：
- 核心 0：WiFi/BT、I2C、输入、物理、debug console
- 核心 1：纯渲染，无中断（除 DMA 完成中断）

**验证**：
- 模拟器：代码审查确认参数
- 真机：`state` 命令显示任务核心亲和性

### 7.3 优化 3：仿射纹理映射选项（API 层面）

**问题**：透视校正 `1.0/fzl` 每 8 像素一次 float 除法

**方案**：API 层面提供两种光栅化路径，由测试场景（游戏引擎层）决定使用哪种：

```c
// ztriangle.c — 提供两个独立函数
void ZB_fillTriangleMappingPerspective(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2);
void ZB_fillTriangleMappingAffine(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2);

// 测试场景（tinygl_test.c）根据需求选择
// 例如：天空盒大面用透视，小物体用仿射
```

**实现方式**：
- `ZB_fillTriangleMappingAffine`：使用 `INTERP_ST`（s, t 线性插值），无 1/z 计算
- `ZB_fillTriangleMappingPerspective`：使用 `INTERP_STZ`（sz, tz 插值），每像素 1/z

**测试场景控制示例**：
```c
// 在测试场景的绘制代码中
if (use_affine_for_this_object) {
    // 仿射映射路径 — 由场景/引擎层决定
    // 适用于：小三角形、远处物体、性能敏感场景
} else {
    // 透视校正路径 — 默认
    // 适用于：大面、近处物体、质量敏感场景
}
```

**验证**：
- 模拟器：分别测试两种路径的像素输出
- 模拟器：仿射路径与透视路径对比，确认纹理坐标误差 < 1 像素
- 真机：测试场景切换两种路径，测量 FPS 差异

### 7.4 优化 4：1/z LUT

**方案**：
```c
#define Z_INV_LUT_SIZE 1024
static uint16_t z_inv_lut[Z_INV_LUT_SIZE];  // Q16.16 定点

// 运行时查表
uint16_t z_inv = z_inv_lut[(z >> (ZB_POINT_Z_FRAC_BITS - 10)) & (Z_INV_LUT_SIZE - 1)];
float zinv = (float)z_inv / 65536.0f;
```

**验证**：
- 模拟器：对比 LUT 与 float 除法的像素输出
- 差异应 < 1/256（纹理坐标 1 像素误差）

### 7.5 优化优先级

| 优化 | 顺序 | 模拟器验证 | 真机验证 | 风险 |
|------|------|-----------|---------|------|
| 双 framebuffer | P1 | 指针 swap 逻辑 | FPS +30% | 低 |
| 核心绑定 | P1 | 代码审查 | 稳定性 | 低 |
| 仿射纹理 | P2 | 像素差异 < 1% | FPS +20% | 中（API 设计）|
| 1/z LUT | P3 | 纹理误差 < 1px | FPS +10% | 低 |

---

## 8. 实施计划

### Phase 1：基础设施（1-2 天）

1. 创建 `tools/tinygl_emu/` 目录结构
2. 实现 `esp_compat.h` + `esp_heap_caps.h` shim
3. 实现 `emu_display.c`（SDL2 backend）
4. 编译现有 TinyGL 到 PC，验证像素一致
5. 实现 `test_rasterizer.c` L1 单元测试

### Phase 2：双 Buffer + 核心绑定（1 天）

1. 修改 `display_st7735.c` 支持双 framebuffer
2. 实现异步 DMA flush
3. 修改 `tinygl_test.c` 核心绑定
4. 模拟器验证逻辑正确
5. 烧机验证 FPS 提升

### Phase 3：纹理优化（1-2 天）

1. 实现仿射纹理映射选项
2. 模拟器对比透视 vs 仿射像素差异
3. 实现 1/z LUT
4. 模拟器验证纹理坐标精度
5. 烧机验证 FPS 提升

### Phase 4：Agent 集成（1 天）

1. 实现 Agent hook + analyze + report
2. 配置文件系统 watch 触发
3. 集成到 `.claude/` memory
4. 端到端测试：改代码 → Agent 自动测试 → 报告

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| 模拟器像素不完全精确 | 测试误报/漏报 | 用真机截图校准，允许 1-bit 差异阈值 |
| 双 buffer DMA 时序复杂 | 画面撕裂 | 模拟器验证 swap 逻辑，真机测试多种场景 |
| 仿射映射视觉不可接受 | 用户投诉 | 混合策略（天空盒仿射，几何透视） |
| 核心绑定导致 watchdog | 系统崩溃 | 保留 yield 点，测试长时间运行 |

---

## 10. 附录

### A. 现有关键文件

| 文件 | 作用 |
|------|------|
| `components/tinygl/src/ztriangle.h` | 扫描线光栅化模板 |
| `components/tinygl/src/ztriangle.c` | 三角形填充实现 |
| `components/tinygl/include/zfeatures.h` | 编译特性开关 |
| `components/tinygl/include/zbuffer.h` | ZBuffer 结构 + 像素宏 |
| `main/tinygl_test.c` | 渲染循环 + 场景 |
| `main/display_st7735.c` | ST7735 display backend |
| `components/hardware/hw_display.c` | SPI2 DMA 驱动 |

### B. 现有关键宏

| 宏 | 值 | 说明 |
|---|-----|------|
| `TGL_PIXEL_BYTE_SWAP` | 1 | RGB565 大端字节序 |
| `TGL_TEXTURE_BYTE_SWAP` | 1 | 纹理大端字节序 |
| `TGL_FEATURE_LIT_TEXTURES` | 0 | 禁用光照纹理（精度 bug） |
| `TGL_FEATURE_16_BITS` | 1 | 16 位渲染模式 |
| `NB_INTERP` | 8 | 透视校正 8 像素展开 |
| `ZB_POINT_Z_FRAC_BITS` | 14 | Z 深度定点精度 |

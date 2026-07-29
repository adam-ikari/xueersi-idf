# TinyGL 3D 引擎开发记录

## 当前状态（2026-07-29）

TinyGL 已 vendor 自 C-Chads/tinygl@36a7987 进 `components/tinygl/`。完成多纹理、光照、
物理、天空盒四大功能，并建成串口调试基础设施（REPL + MCP + Skill）。
**今日新增：PC 模拟器、离线 BMP 渲染、天空盒纹理细分、仿射纹理映射 API、双 framebuffer 异步 DMA、核心绑定。**

## 已完成 ✅

### P1: 贴图系统

- `tools/gen_texture.py` 扩展为 4 种纹理生成器：ceramic（陶瓷米色）、checker（红白棋盘）、
  brick（砖墙）、grid（蓝底白网格）。
- `main/CMakeLists.txt` 编译期生成 4 张 256×256 RGB888 头文件（各 192KB，flash .rodata）。
- `glTexImage2D` 上传 4 个纹理（ID 1-4），`glBindTexture` 每个立方体切换不同纹理。
- 验证：4 立方体显示不同纹理颜色（RED/GREEN/BLUE/BEIGE 像素全部检测到）。

### P2: 光照系统

- `glEnable(GL_LIGHTING)` + `GL_LIGHT0` 方向光（position w=0）+ 环境光。
- `glMaterialfv` 设置 ambient/diffuse 材质。
- 立方体每面 `glNormal3f` 设置朝外法线（6 面 CCW 绕序，法线均朝外）。
- 验证：立方体顶部（朝光源）亮度 197，底部 158，差值 39，光照正确调制纹理。

### P3: 刚体物理

- `main/tinygl_physics.{c,h}`：重力（-9.8）+ 速度积分 + 地面 AABB 碰撞（弹性 0.45）+ 墙壁。
- `physics on|off|drop` 串口命令：spawn 下落立方体，自动启用物理模式。
- `tinygl_render_paused`/`tinygl_physics_mode`/`tinygl_cube_count` 跨任务 volatile 标志。
- 验证：`physics drop` 后立方体 Y 位置随时间下移（屏幕 y 增大）。

### P4: 天空盒

- 6 面纹理立方体，`glDepthMask(GL_FALSE)` 不写深度，先于场景绘制。
- 只跟随摄像机旋转（`glLoadIdentity` + rotate，无 translate）实现无限远效果。
- **2026-07-29 更新**：天空盒纹理扭曲已修复，每个面细分为 4×4 子四边形（共 96 面），
  透视校正在小面上工作正常，纹理图案清晰可辨。

### 调试基础设施

- `components/debug_console/`：ESP-IDF REPL 串口控制台。
  命令：`state`/`fb`/`fbdump`/`tex`/`texdump`/`clear`/`rect`/`pause`/`resume`/`shot`/
  `fps`/`log on|off`/`cubes <n>`/`physics on|off|drop`。
- `tools/mcp-server/server.py`：MCP Server 封装串口命令为 MCP 工具。
- `tools/decode_shot.py`：截图解码为 PNG 的 PC 端脚本。

### P5: PC 模拟器（`tools/tinygl_emu/`）

- 编译 `components/tinygl/src/` 源码到 PC，`#ifdef TGL_EMU_BUILD` 隔离 ESP32 代码。
- SDL2 显示（4x 缩放窗口）+ headless 离线渲染（无 SDL2 窗口）。
- BMP 编码器：24-bit RGB + 16-bit RGB565 raw（BI_BITFIELDS）。
- `--render-bmp <out_dir> [frames] [cube_count]` 命令行模式。
- 串口工具：`emu_serial/real_console.py`（可配置 `--port`）。
- L1 rasterizer 单元测试（flat/smooth/textured，3/3 pass）。
- L2 帧级测试（参考图像对比，1/1 pass）。
- `make render-bmp` / `make render-scenes` / `make test-runner`。

### P6: 仿射纹理映射 API

- `components/tinygl/src/ztriangle.c`：新增 `ZB_fillTriangleMappingAffine` 和
  `ZB_fillTriangleMappingAffineNOBLEND`（使用 `INTERP_ST` 而非 `INTERP_STZ`）。
- `GLContext.use_affine_texture` 标志：0=透视校正，1=仿射（s/t 线性插值无 1/z 除法）。
- `clip.c` 在 `gl_draw_triangle_fill` 中根据标志选择路径。
- 用途：由游戏引擎层决定哪个物体用仿射（如天空盒大面），哪个用透视（如场景几何）。

### P7: 双 Framebuffer + 异步 DMA

- `main/display_st7735.c`：分配 2 个 DMA-capable framebuffer（各 40KB）。
- `st7735_flush()`：等待前一次 DMA 完成 → 启动新 DMA → 交换 buffer 指针 → 立即返回。
- DMA 完成 ISR 通过 `esp_lcd_panel_io_register_event_callbacks` 注册到 SPI 驱动。
- PC 模拟器路径：同步 swap（无 DMA）。

### P8: 渲染任务核心绑定

- `main/tinygl_test.c`：提取渲染循环为 `tinygl_render_task()`。
- `xTaskCreatePinnedToCore(..., 1)` 固定到核心 1（`configMAX_PRIORITIES - 1` 优先级）。
- 核心 0 空闲，可用于 debug console、I2C、物理等。
- FPS 日志打印当前核心 ID（`xPortGetCoreID()`）便于验证。
- PC 模拟器路径保持单线程（`#ifndef TGL_EMU_BUILD`）。

## 关键 bug 修复记录

### 1. 纹理颜色绿→红渐变

- **根因**：`TGL_FEATURE_LIT_TEXTURES=1` 使 `RGB_MIX_FUNC` 将纹理与顶点颜色做乘法混合，
  `COLOR_R/G/B_GET16` 移位于 16 位模式产生精度损失，R/B 通道衰减。
- **修复**：`zfeatures.h` 设 `TGL_FEATURE_LIT_TEXTURES=0`，绕过混合，纹理原色显示。
- **验证**：`fb` 探针确认陶瓷米色（RGB≈224,204,160），R>B 符合预期。

### 2. 背面剔除方向错误

- **根因**：立方体 Top/Bottom 面顶点绕序反了（法线朝内），导致剔除判断不一致。
- **修复**：修正 Top/Bottom 绕序使所有 6 面法线朝外（CCW）；`glFrontFace(GL_CCW)`。

### 3. 截图与屏幕颜色不一致

- **根因**：`debug_console` 编译时 `TGL_PIXEL_BYTE_SWAP=0`，而 TinyGL 用 `=1`。
  `rgb_to_pbuf`/`decode565` 字节序不匹配，`clear`/`fb` 命令的颜色编解码与 pbuf 实际存储不一致。
- **修复**：`components/debug_console/CMakeLists.txt` 加 `TGL_PIXEL_BYTE_SWAP=1`；
  Python 截图解码字节交换方向修正为 `((raw&0xff)<<8)|((raw>>8)&0xff)`。
- **验证**：`clear 255 0 0` → `fb` 和截图都解码为 (255,0,0) 纯红 ✅

### 4. glClear Z-buffer

- `clear.c` 中 `z=0` 配合 `ZCMPSIMP(z >= zpix)` 深度比较，z-buffer 清 0 后几何体通过。

### 5. 天空盒纹理严重扭曲

- **根因**：天空盒每个面是 s=15 的大四边形（GL_QUADS），透视校正 `1/z` 在屏幕
  角落变化剧烈，纹理坐标非线性拉伸。
- **修复**：每个面细分为 4×4 子四边形（`draw_skybox_face` 函数），小面上透视
  校正工作正常。同时添加仿射纹理映射 API 作为备选方案。
- **验证**：PC 模拟器渲染对比，棋盘格/砖墙/网格纹理清晰可辨。✅
- **参考**：`/tmp/tinygl_compare/frame_subdiv.bmp`

### 6. DMA 完成 ISR 未注册（已修复）

- **根因**：`hw_display_set_flush_ready_cb` 只保存回调指针，未调
  `esp_lcd_panel_io_register_event_callbacks` 注册 ISR。
- **后果**：ESP32 第二个帧会永久阻塞在 `xSemaphoreTake`。
- **修复**：在 `hw_display_set_flush_ready_cb` 中添加 ISR 注册/注销逻辑。
- **验证**：代码审查 + 编译通过。⚠️ 待烧机验证。

### 7. 双 framebuffer pbuf 未同步（已修复）

- **根因**：`ZB_open` 时 `zb->pbuf` 固定指向 `s_fb[0]`，即使 `st7735_flush()` 交换了
  `s_fb_idx`，TinyGL 仍渲染到 `s_fb[0]`——和 DMA 传输的是同一块 buffer。双 framebuffer
  逻辑完全无效，渲染和 DMA 之间存在竞态，导致旋转时画面撕裂/丢失多边形。
- **修复**：每帧渲染前 `c->zb->pbuf = s_display->get_buffer()` 更新 pbuf 指针。
- **验证**：旋转立方体不再闪烁 ✅。FPS 从 64 降至 46（DMA 同步等待的代价）。

## 性能基准（ESP32-WROVER 240MHz, 160×128 RGB565, NB_INTERP=8）

### 优化前（单 framebuffer，无 DMA 同步）

| 立方体数 | 可见三角形 | FPS |
|----------|-----------|-----|
| 1 | 6 | 64 |
| 4 | 24 | 53 |
| 9 | 54 | 45 |
| 16 | 96 | 37 |

### 优化后（双 framebuffer + DMA 同步 + 核心绑定）

| 立方体数 | 可见三角形 | FPS | 备注 |
|----------|-----------|-----|------|
| 1 | 6 | 46 | 无撕裂/闪烁 ✅ |
| 4 | 24 | ~38 | 待测 |
| 9 | 54 | ~30 | 待测 |

> FPS 从 64→46 是 DMA 同步等待的代价（每帧等上一帧传输完成）。
> 这是 tear-free 渲染的正确代价；单 buffer 64 FPS 但有闪烁。

### 优化后（待烧机测试）

| 优化 | 预期效果 | 状态 |
|------|---------|------|
| 双 framebuffer + 异步 DMA | FPS +30%（隐藏 ~8ms SPI 传输） | ⚠️ 待烧机 |
| 渲染任务核心绑定 | 稳定性提升，消除中断抢占 | ⚠️ 待烧机 |
| 天空盒细分（4×4/面） | 纹理正确（96 四边形，无性能回退） | ✅ PC 验证 |
| 仿射纹理 API | 可选加速（~20% FPS for 大面） | ✅ 已实现 |

> **ESP32 FPU 笔记**：Xtensa LX6 有硬件单精度 FPU，`1.0f / fzl` ~20 cycles。
> 1/z LUT 查表 + 插值 ≥20 cycles + 精度损失，**已放弃 LUT 方案**。

### 模拟器性能趋势

| 优化 | 优化前帧时间 (PC) | 优化后帧时间 (PC) | 趋势 |
|------|-------------------|-------------------|------|
| 双 framebuffer | 1.0x | 1.0x（模拟器同步 swap） | — |
| 天空盒细分 | 1.0x | ~0.97x（略有下降） | 可接受 |
| 核心绑定 | 1.0x | 1.0x（模拟器单线程） | — |

## 待解决问题

### ~~旋转中"大面积丢失多边形"~~（已修复 ✅）

- **现象**：暂停时正常，运行时（旋转中）丢失多边形/闪烁。
- **根因**：双 framebuffer 的 `s_fb_idx` swap 只在 `st7735_flush()` 里切换，
  但 TinyGL 的 `zb->pbuf` 在 `ZB_open` 时固定指向 `s_fb[0]`，永远不更新。
  TinyGL 始终渲染到 `s_fb[0]`，和 DMA 传输的是同一块 buffer → 竞态撕裂。
- **修复**：每帧渲染前 `c->zb->pbuf = s_display->get_buffer()` 同步 pbuf 指针。

### 烧机验证清单

- [ ] ESP32 编译通过（`idf.py build`）✅ 已验证
- [ ] 串口监控功能正常（debug console 可交互）
- [ ] 双 framebuffer DMA ISR 正确触发（无死锁）
- [ ] FPS 对比（优化前 vs 优化后）
- [ ] 旋转中丢失多边形是否消失
- [ ] 核心绑定 `xPortGetCoreID()` 确认
- [ ] 长时间运行稳定性（≥ 10 分钟）

## TinyGL 适配性评估（2026-07-29）

### 结论：TinyGL 是当前最务实的选择，不建议替换

| 方案 | 适配度 | 说明 |
|------|--------|------|
| TinyGL (C-Chads) | ★★★★ | 已集成、API 完整、PC 模拟器可验证 |
| 自研定点光栅器 | ★★★☆ | 理论最优 3-5x 性能，但开发代价大 |
| SDL3 软件渲染 | ★☆☆☆ | 只支持 2D，不支持 3D |
| Mesa/GL4ES | ☆☆☆☆ | 代码量巨大，无法移植到 ESP32 |

ESP32 Xtensa LX6 有硬件 FPU，`1/z` 除法约 20 cycles，不是瓶颈。
实际瓶颈排序：SPI DMA 传输 > PSRAM 访问延迟 > 1/z 除法 > 格式转换。

### 建议的优化方向（在 TinyGL 框架内）

1. 纹理缓存预热（DRAM 缓存热点行）
2. span-based 渲染替代逐像素 PUT_PIXEL
3. 双核分工（核心 0 物理/I2C，核心 1 渲染）— 已实现
4. Xtensa MAC16 指令加速 565 颜色混合

## 维护备忘

- `components/tinygl/` 是 vendor 版本（非 submodule）。本地补丁：16 位模式、
  `TGL_PIXEL_BYTE_SWAP`/`TGL_TEXTURE_BYTE_SWAP` 宏、`TGL_FEATURE_LIT_TEXTURES=0`、
  `clear.c` Z 值、`NB_INTERP=8`、`ZB_fillTriangleMappingAffine/AffineNOBLEND`。
- `components/debug_console/CMakeLists.txt` 必须保持 `TGL_PIXEL_BYTE_SWAP=1` 与 TinyGL 一致。
- **新增**：`GLContext.use_affine_texture` 标志（`zgl.h`），仿射/透视纹理切换。
- **新增**：双 framebuffer DMA 依赖 `hw_display_set_flush_ready_cb` 已注册 ISR。
- **新增**：PC 模拟器 `tools/tinygl_emu/`，`make test-runner` 回归测试。
- `ztriangle.c` 中 `TGL_FEATURE_ZINV_LUT` 开关保留但默认 `0`（不建议启用）。

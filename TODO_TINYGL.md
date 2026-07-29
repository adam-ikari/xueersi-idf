# TinyGL 3D 引擎开发记录

## 当前状态（2026-07-29）

TinyGL (C-Chads) vendor 至 `components/tinygl/`。完成多纹理、光照、天空盒三大功能，
建成串口调试基础设施（REPL + MCP + Skill）+ PC 模拟器离线验证体系。
单 framebuffer + DMA 同步等待 = 29 FPS tear-free（1 立方体 + 天空盒场景）。

## 已完成 ✅

### P1: 贴图系统
- 4 纹理（ceramic/checker/brick/grid）+ 编译期 `gen_texture.py` 生成 256×256 RGB888。
- 多纹理绑定（`glBindTexture`），每个立方体不同纹理。

### P2: 光照系统
- `GL_LIGHT0` 方向光 + 环境光，Gouraud shading + `GL_SMOOTH`。
- `RGB_MIX_FUNC` 修复：un-swap 后提取通道 → LIT_TEXTURES=1 颜色正确 ✅
- 方向光 + 陶瓷纹理 = 顶部亮/底部暗的暖色渐变。

### P3: 刚体物理
- `tinygl_physics.{c,h}`：重力 + AABB 碰撞 + 弹性。
- `physics drop` 串口命令：spawn 下落立方体。

### P4: 天空盒
- 6 面纹理立方体，`glDepthMask(GL_FALSE)`，跟摄像机旋转不跟随位移。
- 4×4 面细分（96 四边形）修扭曲 → 棋盘格/砖墙清晰可辨。

### 调试基础设施
- `components/debug_console/`：REPL（`fb/tex/state/shot/pause/resume/clear/rect/cubes/fps`）。
- `tools/mcp-server/server.py` + `tools/decode_shot.py`。
- `tools/tinygl_emu/`：PC 模拟器 + SDL2 显示 + headless BMP 渲染 + 测试套件。

### 关键性能优化
- 单 framebuffer + DMA 同步等待（`wait_dma` before render）→ tear-free 无闪烁 ✅
- 渲染任务核心绑定 core 1 ✅
- 30 FPS 精确帧率锁定（render + DMA + fine-spin limiter）✅
- 仿射纹理 API（`use_affine_texture`）✅

## 关键 bug 修复记录

### 1. 纹理颜色绿→红渐变 → LIT_TEXTURES RGB_MIX_FUNC
- **根因**：`GET_RED/GREEN/BLUE` 掩码假设逻辑 565，但 `TGL_PIXEL_BYTE_SWAP=1` 下 tpix 已交换。
- **修复**：`RGB_MIX_FUNC` 中 `_TGL_UNSWAP16(tpix)` 后提取通道（GCC 语句表达式）。

### 2. 背面剔除方向
- 修正立方体 Top/Bottom 绕序（法线朝外），`glFrontFace(GL_CCW)` ✅

### 3. 截图/屏幕颜色不一致
- `debug_console` `TGL_PIXEL_BYTE_SWAP` 同步 + 解码反向 ✅

### 4. 双 framebuffer pbuf 不同步 → 切换单 fb
- 双 fb 的 `s_fb_idx` swap 对 TinyGL 无效（`zb->pbuf` 固定指向 `s_fb[0]`）。
- 改为单 fb + DMA 同步等待（`wait_dma` before render）→ 61 FPS 无闪烁。

### 5. DMA ISR 注册修复 → `hw_display_set_flush_ready_cb` ✅

### 6. 天空盒纹理扭曲 → 4×4 面细分 ✅

### 7. 色彩抖动实验
- **glPostProcess 回调**：颜色扭曲 + FPS 暴跌（614K 回调/秒 ❌）
- **内联 Bayer 4×4**：黑色网点（幅度太大 ❌）
- **内联 2×2 ±1 LSB**：颜色仍不对（量化后抖动无效 ❌）
- **结论**：后处理抖动对 16 位无效（误差已在 RGB_TO_PIXEL 中丢失）。
  正确做法需在光栅器内部（PUT_PIXEL）操作 24 位内部颜色。
  → **P2 待办**：`GLContext.dither_enabled` 标志 + `PUT_PIXEL` 宏中嵌入抖动。

### 8. 环境贴图反射
- 应用层 `draw_reflective_cube()`：sphere-map 逐顶点反射向量 → 纹理坐标。
- 效果有限（逐顶点），需魔改 TinyGL 管线支持逐像素法线插值。

## 性能基准（ESP32-WROVER 240MHz，160×128 RGB565，1 立方体+天空盒）

| 配置 | FPS | 备注 |
|------|-----|------|
| 单 fb + DMA 同步 + 锁 30fps | 29-30 | tear-free ✅ |
| 单 fb + DMA 同步（无锁） | ~60 | 观感不如 30fps |
| 双 fb（pbuf 未同步） | 64 | 闪烁 ❌ |

## 待开发

### P1: 定点数优化
- ESP32 有硬件 FPU，`1/z` 约 20 cycles，非瓶颈。但 `COLOR_MULT_MASK` 的 24 位整数运算
  仍有优化空间。
- 方向：仿射纹理路径（`use_affine_texture`）已移除 `1/z` 除法，下一步移除浮点 s/t 插值。

### P2: 光栅器抖动
- 在 `PUT_PIXEL` 宏中做 ordered dithering（不是后处理）。

### P3: 实时阴影
- shadow mapping 或 stencil shadow volumes（TinyGL 无 stencil buffer，需 shadow map）。

### P4: 游戏引擎集成
- 场景图 + 相机系统 + 动画循环
- WAMR Host API：TinyGL + Canvas 2D 混合渲染

## 维护备忘
- `components/tinygl/` 是 vendor（非 submodule）。本地补丁：
  `TGL_PIXEL_BYTE_SWAP`/`TGL_TEXTURE_BYTE_SWAP`、`TGL_FEATURE_LIT_TEXTURES=1`、
  `NB_INTERP=8`、`ZB_fillTriangleMappingAffine/AffineNOBLEND`、`RGB_MIX_FUNC` un-swap。
- `components/debug_console/CMakeLists.txt` 必须 `TGL_PIXEL_BYTE_SWAP=1`。
- PC 模拟器 `tools/tinygl_emu/`，`make test-runner`。

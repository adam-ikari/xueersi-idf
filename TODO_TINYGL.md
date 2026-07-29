# TinyGL 3D 引擎开发记录

## 当前状态（2026-07-29）

TinyGL 已 vendor 自 C-Chads/tinygl@36a7987 进 `components/tinygl/`。完成多纹理、光照、
物理、天空盒四大功能，并建成串口调试基础设施（REPL + MCP + Skill）。

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
- `glDisable(GL_CULL_FACE)` 渲染所有内表面。
- 验证：100% 纹理覆盖，0% 黑色/裁切，无 clear 色残留。

### 调试基础设施

- `components/debug_console/`：ESP-IDF REPL 串口控制台。
  命令：`state`/`fb`/`fbdump`/`tex`/`texdump`/`clear`/`rect`/`pause`/`resume`/`shot`/
  `fps`/`log on|off`/`cubes <n>`/`physics on|off|drop`。
- `tools/mcp-server/server.py`：MCP Server 封装串口命令为 MCP 工具。
- `.claude/skills/xiaomiao-debug.md`：TinyGL 调试 Skill。
- `tools/decode_shot.py`：截图解码为 PNG 的 PC 端脚本。

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

## 性能基准（ESP32-WROVER 240MHz, 160×128 RGB565, NB_INTERP=8）

| 立方体数 | 可见三角形 | FPS |
|----------|-----------|-----|
| 1 | 6 | 64 |
| 4 | 24 | 53 |
| 9 | 54 | 45 |
| 16 | 96 | 37 |

## 待解决问题

### 旋转中"大面积丢失多边形"

- **现象**：暂停时正常，运行时（旋转中）丢失多边形。
- **已排除**：渲染算法（截图 98-99% 覆盖，0% 黑色）、近裁切、深度测试。
- **怀疑方向**：
  - DMA flush 异步（`trans_queue_depth=10`），下一帧渲染覆盖正在传输的 pbuf？
    但渲染 15ms > DMA 8ms，理论上不重叠。
  - 帧间 GL 状态泄漏（天空盒 `glDepthMask`/`glCullFace` 恢复不完整？）。
  - Z-buffer 清除在某些角度不可靠。
- **下一步**：加 zbuf[0] 帧间诊断，确认 `glClear` 每帧生效；检查天空盒状态恢复。

### 天空盒纹理扭曲

- 大面（s=15）透视校正纹理映射在角落 1/z 变化剧烈，纹理拉伸。
- 考虑：天空盒专用投影矩阵、或简化为纯色渐变。

## 维护备忘

- `components/tinygl/` 是 vendor 版本（非 submodule）。本地补丁：16 位模式、
  `TGL_PIXEL_BYTE_SWAP`/`TGL_TEXTURE_BYTE_SWAP` 宏、`TGL_FEATURE_LIT_TEXTURES=0`、
  `clear.c` Z 值、`NB_INTERP=8`。
- `components/debug_console/CMakeLists.txt` 必须保持 `TGL_PIXEL_BYTE_SWAP=1` 与 TinyGL 一致。

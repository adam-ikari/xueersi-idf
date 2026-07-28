# TinyGL 纹理颜色修复 TODO

## 当前状态
纹理已成功映射到立方体表面（绿到红渐变），但颜色不符合预期（应为陶瓷米色）。
`TGL_OPTIMIZATION_HINT_BRANCH_COST=2` bug 已修复，纹理坐标插值正常工作。

## 待修复问题

### 1. 纹理颜色映射
- 现象：立方体显示绿→红渐变，而非预期陶瓷色
- 分析：`gl_convertRGB_to_5R6G5B` 中的字节交换方向或 RGB 通道顺序可能有问题
- 方法：PC 端生成已知颜色的纹理 BMP，ESP32 渲染后 dump 帧缓冲对比

### 2. 纹理格式通用化
- 当前 `PIXEL_SWAP16` 宏在 `zbuffer.h` 中硬编码 `TGL_PIXEL_BYTE_SWAP=1`
- 需要从 Kconfig 或 display_backend 动态读取像素格式
- 支持：RGB565、RGB565_SWAP、RGBA8888

### 3. 纹理尺寸限制
- TinyGL 强制 256×256 纹理（`TGL_FEATURE_TEXTURE_DIM`）
- 需要支持任意尺寸纹理（已有 `gl_resizeImageNoInterpolate` 但未启用）
- 或在打包工具中预处理纹理到 256×256

### 4. 纹理性能优化
- 当前纹理采样每像素做定点数除法/乘法
- 考虑：mipmap、最近邻采样优化
- 纹理缓存：256×256×2 = 128KB，已在 PSRAM 分配

### 5. 多纹理支持
- 当前只绑定纹理 ID 1
- 需要 `glBindTexture` 多纹理切换
- 纹理图集（texture atlas）支持

## 下一阶段工作

### P3: 游戏启动器 + 资源加载
- TF 卡 .wasm/.aot 加载
- 游戏列表 UI
- .ximg 贴图加载器
- .xmid 音符播放器

### P4: WAMR 集成
- WAMR AOT 运行时
- Host API 注册（Canvas 2D + 输入 + 音频）
- 游戏循环
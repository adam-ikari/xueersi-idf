# WAMR 游戏运行时 + 完整游戏机 API 设计

**日期：** 2026-07-16
**范围：** 完整游戏机软件生态：游戏包格式、PC 安装器、WAMR 运行时、Canvas 2D 图形 API、音频 API、输入 API

---

## 1. 游戏机软件生态架构

```
游戏开发者
  │
  ├── 输出游戏包 (.xpk) — 通用中间格式
  │     .wasm (通用 WASM 字节码)
  │     .ximg 源 (RGBA8888 源图 — 中间格式)
  │     .mid 源 (标准 MIDI — 中间格式)
  │     manifest.json (硬件需求 + 增强信息)
  │
  └── 上传到游戏商店/分享
        │
        ▼
用户下载游戏包 (.xpk)
  │
  ▼
PC 安装器
  │
  ├── 1. 读取 manifest.json
  ├── 2. 检测游戏机型号 / 硬件 profile
  ├── 3. 资源适配与增强
  │     ├── 图像：源格式 → 抖动/缩放/量化 → 目标格式
  │     ├── 音频：MIDI → 解析 → 目标格式
  │     └── WASM：可选 AOT 编译
  ├── 4. 写入 TF 卡 (/sdcard/games/<game_id>/)
  └── 5. 注册游戏到游戏列表
```

## 2. 游戏格式版本模型

```
.xpk 格式版本：

v1 格式（本代基准）
  .wasm + .ximg (Indexed8/RGB565) + .xmid (音符事件序列)
  ├── v1 基础版 → 安装器转换输出基础格式
  └── v1 增强版 → 安装器输出更高品质（更高色深、分辨率）

v2 格式（次世代）
  .wasm + .ximg (RGBA8888) + .xmid (多声道)
  ├── v2 游戏机 → 安装器直接输出原生格式
  └── v1 游戏机 → 安装器降级适配（抖动 RGB565）

v3+ 未来格式
  ├── 新游戏机 → 原生支持
  └── 旧游戏机 → 安装器尝试降级，否则提示不兼容
```

**核心原则：** 游戏包是通用中间格式，不限定具体硬件。安装器根据硬件 profile 转换资源。

## 3. PC 安装器

### 功能
- 读取 `.xpk` 包
- 检测游戏机型号 / 硬件配置
- 根据硬件 profile 转换资源
- 写入 TF 卡
- 注册游戏到游戏列表

### 硬件 profile 示例
```json
{
    "xiaomiao_v1_base": {
        "screen": "160x128_rgb565",
        "audio": "pwm_tone",
        "psram_kb": 4096,
        "cpu_mhz": 240
    },
    "xiaomiao_v1_plus": {
        "screen": "320x240_rgb888",
        "audio": "i2s_mono",
        "psram_kb": 8192,
        "cpu_mhz": 400
    }
}
```

## 4. WAMR 运行时（ESP32 固件）

### 组件
- `espressif/wasm-micro-runtime`（ESP-IDF 组件注册表）
- 启用 AOT 支持
- 解释器模式作为 fallback
- 启用 WASI（文件系统访问 `/sdcard`）
- 堆大小：128KB–256KB（PSRAM）

### 运行时架构
```
WAMR 运行时
  │
  ├── 加载 .aot/.wasm
  ├── 注册 Host API（通过 wasm_runtime_register_natives）
  │     ├── xiaomiao/canvas_*     — 图形 API
  │     ├── xiaomiao/audio_*      — 音频 API
  │     ├── xiaomiao/input_*      — 输入 API
  │     ├── xiaomiao/storage_*    — 文件系统 API
  │     └── xiaomiao/hardware_*   — 硬件控制 API
  ├── 执行游戏循环
  └── 游戏退出后卸载
```

## 5. 完整 Host API 清单

### 5.1 图形 API (Canvas 2D)

**渲染状态控制：**
```c
void canvas_begin_frame(void);           // 清除帧缓冲
void canvas_end_frame(void);             // flush 到显示屏
```

**颜色与样式：**
```c
void canvas_set_fill_style(int r, int g, int b, int a);
void canvas_set_stroke_style(int r, int g, int b, int a);
void canvas_set_global_alpha(float alpha);
void canvas_set_line_width(float width);
```

**矩形：**
```c
void canvas_fill_rect(float x, float y, float w, float h);
void canvas_clear_rect(float x, float y, float w, float h);
void canvas_stroke_rect(float x, float y, float w, float h);
```

**路径：**
```c
void canvas_begin_path(void);
void canvas_move_to(float x, float y);
void canvas_line_to(float x, float y);
void canvas_stroke(void);
void canvas_fill(void);
void canvas_close_path(void);
void canvas_arc(float x, float y, float r, float start_angle, float end_angle);
void canvas_rect(float x, float y, float w, float h);
```

**文本：**
```c
void canvas_set_font(const char *font_spec);
void canvas_fill_text(const char *text, float x, float y);
void canvas_set_text_align(int align);  // 0=left, 1=center, 2=right
int  canvas_measure_text(const char *text);  // 返回像素宽度
```

**贴图：**
```c
int  canvas_load_image(const char *path);  // 返回 image_id，-1=失败
int  canvas_image_get_width(int img);
int  canvas_image_get_height(int img);
void canvas_draw_image(int img, float x, float y);
void canvas_draw_image_scaled(int img, float x, float y, float w, float h);
void canvas_draw_image_frame(int img, float x, float y, int fx, int fy, int fw, int fh);
void canvas_draw_image_frame_scaled(int img, float x, float y, float w, float h, int fx, int fy, int fw, int fh);
void canvas_unload_image(int img);
```

**像素操作：**
```c
void canvas_put_pixel(float x, float y, int r, int g, int b, int a);
void canvas_get_pixel(float x, float y, int *r, int *g, int *b, int *a);
```

**变换：**
```c
void canvas_translate(float dx, float dy);
void canvas_rotate(float angle);
void canvas_scale(float sx, float sy);
void canvas_save(void);
void canvas_restore(void);
void canvas_reset_transform(void);
```

### 5.2 音频 API

**当前硬件（PWM 蜂鸣器）：**
```c
void audio_play_tone(int freq, int duration_ms);
void audio_stop(void);
```

**预留接口（将来 I2S 硬件）：**
```c
int  audio_play_midi(const uint8_t *data, int len);  // -1 = 不支持
void audio_stop_midi(void);
int  audio_is_midi_playing(void);
void audio_set_volume(int vol);  // 0-100
```

### 5.3 输入 API

```c
// 事件风格（每帧查询）
int  input_get_key(int key_id);    // 返回 0/1 当前按下状态
void input_clear_events(void);     // 清空事件队列

// 按键 ID
#define KEY_UP     0
#define KEY_DOWN   1
#define KEY_LEFT   2
#define KEY_RIGHT  3
#define KEY_A      4
#define KEY_B      5
```

### 5.4 存储 API

```c
int  storage_load(const char *path, uint8_t *buf, int max_len);  // 返回实际长度
int  storage_save(const char *path, const uint8_t *buf, int len); // 保存存档
int  storage_get_size(const char *path);  // 返回文件大小，-1=不存在
int  storage_delete(const char *path);    // 删除文件
```

### 5.5 硬件控制 API

```c
void hardware_set_led(int index, int on);    // 0/1
void hardware_set_motor(int index, int speed, int dir);  // 0-255, 0/1
void hardware_get_battery(int *percent);     // 预留
```

## 6. `.ximg` 贴图格式 v1

```
Magic:      "XIMG" (4 bytes)
Version:    uint32 = 1
Width:      uint16
Height:     uint16
PixelFormat: uint8
  0x01 = Indexed8 (8位调色板)
  0x02 = RGB565 (16位直接色)
  0x03 = RGBA8888 (32位，预留)
  0x10+ = 保留扩展
Flags:      uint8
  bit 0 = 有透明色
  bit 1 = 有调色板
  bit 2-7 = 保留
PaletteSize: uint16 (1-256, 0=无)
[调色板数据] PaletteSize × 2 bytes (RGB565)
[可选] TransparentIndex: uint8 (Flags bit 0=1时)
[像素数据] 见 PixelFormat
[可选] 扩展块链表 (大小+类型+数据，0=结束)
```

## 7. `.xmid` 音符事件格式 v1

```
Magic:      "XMID" (4 bytes)
Version:    uint32 = 1
BPM:        uint16
TrackCount: uint8
[事件表]
  N × 8 bytes:
    delta_time_ms: uint16
    note:  int8 (0-127, -1=结束)
    velocity: uint8
    duration_ms: uint8
    reserved: 3 bytes
```

## 8. `.xpk` 游戏包格式

```
[Header]
  Magic: "XPK1" (4 bytes)
  FormatVersion: uint32
  ResourceCount: uint32
  MetadataOffset: uint32
  MetadataSize: uint32
[Index Table]
  N × 80 bytes:
    filename[64]: char
    type: uint8 (0=AOT, 1=WASM, 2=XIMG, 3=XMID, 4=MID)
    data_offset: uint32
    data_size: uint32
[Metadata] JSON
  game.id, title, format_version, hardware_requirements, resources[...]
[Data Blocks]
  .aot/.wasm, .ximg, .xmid 数据依次排列
```

## 9. 不在本次范围

- I2S 音频硬件
- 游戏商店/分发平台
- 游戏开发工具（SDK/打包工具）
- 多任务/游戏后台管理 UI
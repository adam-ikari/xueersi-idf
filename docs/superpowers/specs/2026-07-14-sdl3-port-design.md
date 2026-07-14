# SDL2/SDL3 底座移植 + 全硬件自检分页 demo 设计

**日期：** 2026-07-14
**范围：** 本次任务只做 SDL3 底座 + 一个 SDL3 全硬件自检分页 demo。wasm3 运行时 / 游戏环境不在本次范围，但底座接口形态为 wasm3 宿主 import 调用预留。

## 背景与目标

小喵掌机现有 ESP32 固件是一个 LVGL 硬件状态 dashboard，全部逻辑（硬件驱动 + UI）写在单文件 `main/main.c`（~2200 行）。

本次目标：
1. **删除 LVGL**，把渲染/输入底座换成 **SDL3**（SDL2 的继任者，API 几乎兼容；将来 wasm3 游戏侧用 SDL2 或 SDL3 都能桥接）。
2. **解耦硬件层** —— 把所有硬件驱动从 `main.c` 抽成可复用模块，留 `hw_*.h` 接口。这是长期资产：将来 wasm3 宿主直接调这些接口驱动硬件。
3. **SDL3 全硬件自检分页 demo** —— 用 SDL3 复刻现有 dashboard 的分页模型（LIGHT/THERM/MOTION/LED1/LED2/BUZZER/MOTOR1/MOTOR2/SD/GPIO25/GPIO26/ADC32/ADC33/SYSTEM/ABOUT），D-pad 翻页 + A/B 操作，在硬件上验证所有外设 + SDL3 渲染都通。

## 未来架构（上下文，非本次实现）

```
TF 卡 (.wasm 游戏)
   │  文件系统加载
   ▼
wasm3 运行时
   │  wasm → host import 调用
   ▼
SDL3 (图形 API + 输入)        ← 游戏代码用 SDL3 画图、读按键
   │
   ▼
ST7735 / SPI2 / GPIO 按键 / 蜂鸣器
```

本次只实现到 SDL3 底座 + demo。音频暂不实现（见 §2）。

## §1 硬件层拆分边界

把现在写死在 `main.c` 的硬件逻辑抽成 `components/hardware/` 下的一组模块，UI 层（SDL3 demo 或将来 wasm3 宿主）只通过头文件调用。按外设分：

| 模块 | 头/源 | 对外接口（示例） | 来源函数 |
|---|---|---|---|
| `hw_display` | `hw_display.h/.c` | `hw_display_init()`、`hw_display_flush(area, px_map)`、`hw_display_on()` —— **暴露给 SDL3 BSP board 适配用** | `lcd_init`/`st7735_*`/`lcd_display_on`/`lvgl_flush_cb` 的 SPI 部分 |
| `hw_input` | `hw_input.h/.c` | `hw_input_init()`、`hw_input_poll(out)` —— 6 按键映射 | `buttons_init`/`keypad_read_cb` |
| `hw_i2c` | `hw_i2c.h/.c` | `hw_i2c_init()`、bus/dev handle 访问 | `i2c_init`/`i2c_write`/`i2c_write_reg`/`i2c_read_reg` |
| `hw_gd32` | `hw_gd32.h/.c` | `hw_gd32_probe()`、`hw_gd32_set_led(idx,bool)`、`hw_gd32_motor_set(motor,dir,speed)`、`hw_gd32_motor_stop_all()` | `gd32_*` 全族 |
| `hw_mpu` | `hw_mpu.h/.c` | `hw_mpu_probe()`、`hw_mpu_read(acc,gyro,pitch,roll,gesture)` | `mpu_*` 全族 |
| `hw_adc` | `hw_adc.h/.c` | `hw_adc_init()`、`hw_adc_read_light()`、`hw_adc_read_temp()`、`hw_adc_read_ext(idx)` | `adc_*` 全族 |
| `hw_buzzer` | `hw_buzzer.h/.c` | `hw_buzzer_init()`、`hw_buzzer_beep(freq,ms)`、`hw_buzzer_stop()` | `buzzer_*` |
| `hw_extio` | `hw_extio.h/.c` | `hw_extio_init()`、`hw_extio_set(idx,bool)`、`hw_extio_set_pwm(idx,duty)` | `ext_io_init`/`ext_output_set`/`ext_pwm_*` |
| `hw_sd` | `hw_sd.h/.c` | `hw_sd_try_mount()`、`hw_sd_unmount()`、`hw_sd_info(name,mb)` | `sd_try_mount`/`sd_unmount` |
| `hw_board` | `hw_board.h/.c` | 顶层 `hw_board_init()`（调所有 init）、`hw_board_update()`（轮询+reprobe）、`hw_board_state_t`（= 现 `board_state_t`）UI 只读全局状态 | `hardware_init`/`hardware_update`/`hardware_process_timers` |

原则：
- **状态集中在 `hw_board_state_t`**（沿用现 `board_state_t`），UI 只读。保留单状态 struct 模型，避免散落全局变量。
- `hw_display` 是**唯一**碰 SPI2/ST7735 的模块；SDL3 BSP board 适配通过它 flush，不重复 SPI 初始化。
- 所有 `#define` 常量（引脚、寄存器、I2C 地址）移到对应 `hw_*.h`，集中管理。

## §2 SDL3 集成方式与 BSP board 适配

**依赖引入：**
- `main/idf_component.yml` 加 `georgik/sdl`（SDL3 主体）。
- `georgik/esp-idf-component-SDL_bsp` git clone 进 `components/esp_bsp_sdl/`（组件库不放，得本地 clone）。

**新增板子适配 `esp_bsp_sdl_xiaomiao.c`**（放 `esp_bsp_sdl/src/boards/`，仿照最接近的 M5 Atom S3 那块 128×128 SPI），三件事：
1. **video backend**：`esp_bsp_sdl_init()` 不重新初始化 SPI，调用 `hw_display` 拿现成 `esp_lcd_panel_io_handle_t`。SDL3 渲染完一帧后，BSP 通过 `esp_lcd_panel_io_tx_color`（或 `hw_display_flush`）推像素到 ST7735，复用现有三重 DMA buffer 路径。**颜色格式对齐**：SDL3 出 RGB565，屏要 byte-swapped RGB565——在 BSP 适配处理字节序（或让 SDL3 直接出 swapped 格式），具体方案在实现期（P2/P3）决定，二选一即可，不影响架构。
2. **input backend**：`hw_input` 把 6 按键按下/松开转 SDL3 `SDL_KEYDOWN`/`SDL_KEYUP`（UP/DOWN/LEFT/RIGHT→方向键，A→Return/Enter，B→Escape）。
3. **audio backend**：只有蜂鸣器（PWM 单声道），SDL3 PCM 音频无真实出口。**禁用/stub SDL3 audio**（`SDL_Init` 不带 `SDL_INIT_AUDIO`，或后端返回空设备），demo 不依赖音频。将来 wasm3 游戏要音频时再议（可能需加 I2S，另工程）。

**未解风险点（实现第一步需核实）：**
- `esp_bsp_sdl` 的 board `.c` 是直接调 `esp_lcd`，还是绕一层 ESP-BSP 板级抽象（会拽进不必要依赖）。若是后者，适配层变厚——可能需 fork `esp_bsp_sdl` 或写不依赖其上层抽象的最小 board 文件。**这是方案 A 最大不确定点，P2 阶段首先验证。**

## §3 SDL3 demo 的 UI 形态与页面对应

复刻现有 dashboard 分页模型，渲染换 SDL3。页集合与现 `ui_page_t` 一一对应（共 15 页）：

| 页 | 内容 | 调用的硬件层 |
|---|---|---|
| LIGHT | 光照值 + 滚动折线图（RGB565 手绘） | `hw_adc_read_light` + 历史 |
| THERM | 温度值 + 折线图 | `hw_adc_read_temp` + 历史 |
| MOTION | MPU6050 acc/gyro/pitch/roll/gesture | `hw_mpu_read` |
| LED 1 / LED 2 | A 键切换开关 | `hw_gd32_set_led` |
| BUZZER | A 键鸣响 / B 停 | `hw_buzzer_beep` |
| MOTOR 1 / MOTOR 2 | A 键正转/反转/停，B 调速 | `hw_gd32_motor_set` |
| SD CARD | 卡名 + 容量，A 重挂载 | `hw_sd_try_mount`/`hw_sd_info` |
| GPIO25 / GPIO26 | A 键切换输出 / PWM 占空 | `hw_extio_set` / `hw_extio_set_pwm` |
| GPIO32 / GPIO33 (ADC32/33) | ADC 原始值 → 百分比 + 进度条 | `hw_adc_read_ext` |
| SYSTEM | CPU/flash/PSRAM/IDF 版本/空闲堆/每外设 last `esp_err_t` | `hw_board_state_t` 只读 |
| ABOUT | 项目信息 + 翻页滚动 | 静态文本 |

**交互模型**（与现 LVGL 一致）：
- D-pad 左/右翻页，带横向滑动动画（SDL3 自绘，不复用 LVGL 动画）。
- A 键 = action：toggle LED、run/stop motor、beep、toggle ext-IO、重挂 SD。
- B 键 = cancel/adjust：MOTOR 页调速，ABOUT 页滚动。
- 状态显示：屏顶固定一行 GD32/MPU/SD 在线状态；底部临时 action 提示（如 "LED1 ON" ~850ms）。

**渲染策略**：SDL3 软件渲染（`SDL_RENDERER_SOFTWARE` 或操作 `SDL_Surface`），ESP32 无 GPU。每帧：`hw_board_update()` 填状态 → demo 读状态 → 在 160×128 RGB565 SDL surface 用 `SDL_FillRect`/手绘折线/文本 → BSP flush 到屏。~60fps，对齐现 `UI_REFRESH_PERIOD_MS=16`。

**文本**：先用 SDL3 内置最小字体（`SDLTest` 8×8）跑通，省一个依赖。字体美化以后再说，不引 `sdl_ttf`。

## §4 运行模型与线程边界

删 LVGL 后模型更简单。约束保留：**单 worker 线程驱动一切，避免 SDL3 与硬件轮询并发冲突。**

```
app_main
  └─ hw_board_init()          // 所有硬件 init（含 hw_display、SDL3 BSP esp_bsp_sdl_init）
  └─ sdl_demo_create()        // 建 SDL3 window/renderer/surface、初始页
  └─ 启动 esp_timer 1ms tick  // SDL_GetTicks / hw_input 去抖计时基准
  └─ xTaskCreate(demo_task)  // 唯一 worker
        loop:
          1. hw_board_update()           // 轮询传感器 + reprobe GD32/MPU（16ms 节拍）
          2. SDL_PollEvent()             // 拉取 hw_input 推入的按键事件
          3. demo 按页处理事件            // action/cancel/adjust
          4. demo 渲染当前页             // 画到 160×128 RGB565 surface
          5. SDL_RenderPresent()         // BSP flush 到 ST7735
          6. usleep(节拍剩余)
```

**线程边界：**
- 硬件轮询（`hw_board_update`）和 SDL3 事件/渲染都在同一个 `demo_task` 顺序执行——与现 `lvgl_task` 模型一致，不引第二个 sensor 任务。
- `hw_input` 按键去抖在 `hw_input_poll` 内用 `esp_timer_get_time`，不另开任务；事件经 SDL3 事件队列投递（线程安全）。
- `hw_buzzer_beep` 定时停止（现 `s_buzzer_stop_at`）并入 `hw_board_update` 的 timer 处理。
- **wasm3 预留**：将来 wasm3 跑在 `demo_task` 内（或子任务），通过 host import 调 `hw_*`；接口届时若需跨线程访问硬件再加互斥锁。**本次不加锁**（YAGNI，单线程下不需要）。

## §5 文件结构、构建与配置

**新增文件：**
```
main/
  CMakeLists.txt              // 改：去 lvgl 依赖，加 sdl + esp_bsp_sdl + hardware 组件
  main.c                      // 瘦化为 app_main：hw_board_init → sdl_demo_create → demo_task
  sdl_demo.c / sdl_demo.h     // SDL3 全硬件自检分页 demo（UI 渲染 + 事件 + 页面）
  sdl_font.c / sdl_font.h     // 最小点阵字体封装（先用 SDL3 内置 8×8）
components/
  hardware/                   // 解耦的硬件层（§1 的 10 个模块）
    CMakeLists.txt
    include/hw_board.h  hw_display.h  hw_input.h  hw_i2c.h  hw_gd32.h
            hw_mpu.h  hw_adc.h  hw_buzzer.h  hw_extio.h  hw_sd.h
    hw_board.c  hw_display.c  hw_input.c  hw_i2c.c  hw_gd32.c
    hw_mpu.c  hw_adc.c  hw_buzzer.c  hw_extio.c  hw_sd.c
  esp_bsp_sdl/                // git clone georgik/esp-idf-component-SDL_bsp
    src/boards/esp_bsp_sdl_xiaomiao.c   // 本次新增板子适配
    + 改 Kconfig + CMakeLists 加新板选项
```

**配置变更：**
- `main/idf_component.yml`：删 `lvgl/lvgl`，加 `georgik/sdl`。
- `sdkconfig.defaults`：删 `CONFIG_LV_*` 一族；加 SDL3 相关 Kconfig；去掉 `CONFIG_LV_CONF_SKIP`。`SPI_MASTER_IN_IRAM` 保留（flush ISR 仍需要）。
- `dependencies.lock`：重生成。
- `CLAUDE.md`：更新架构描述（LVGL→SDL3，硬件层模块化，wasm3 规划）。

**不删的东西：** 现有 `board_state_t`/`ui_page_t` 概念保留（状态 struct、页枚举），只是从 LVGL widget 变 SDL3 自绘。README 的硬件 spec / GD32 协议 / 引脚表**不动**（仍是硬件真相源）。

## §6 验收标准与分阶段落地

**验收（demo 跑在硬件上成立）：**
1. 编译通过，flash 后串口见 `hw_board_init` 日志、SDL3 初始化日志，无崩溃。
2. 屏幕点亮（沿用"首帧 flush 完再开屏"防闪屏逻辑），显示 LIGHT 页。
3. 左右翻页遍历所有 15 页，滑动动画顺滑。
4. 每页硬件功能实测：LED1/2 点灭、MOTOR1/2 正反转调速停、BUZZER 鸣响、ADC 数值/折线实时更新、MPU6050 姿态、SD 挂载/容量、GPIO25/26 输出/PWM、GPIO32/33 ADC、SYSTEM 页版本与堆。
5. GD32/MPU/SD 离线时状态栏正确显示 ABSENT，reprobe 后恢复在线。
6. ~60fps 不撕裂（无 TE 引脚，轻微撕裂属预期，只要不卡顿）。

**分阶段落地（实现计划细化）：**
- **P1 硬件层解耦**：把 `main.c` 硬件函数抽进 `components/hardware/`，`main.c` 临时调它们验证不回归。先确保硬件行为零回归。
- **P2 引入 SDL3 + BSP 验证**：加 `georgik/sdl` + clone `esp_bsp_sdl`，**首先验证 §2 风险点**——`esp_bsp_sdl` board `.c` 能否薄接 `hw_display` 的 `esp_lcd_panel_io`。决定是否需 fork `esp_bsp_sdl`。
- **P3 最小 SDL3 demo**：屏上画一屏 + 读一按键，证明 video+input backend 通。
- **P4 全硬件分页 demo**：逐页实现 15 页 + 交互，删 LVGL。
- **P5 收尾**：`sdkconfig`/`CLAUDE.md` 更新，merge-bin 出固件。

## 不在本次范围（YAGNI）

- wasm3 移植、wasm 游戏加载、wasi 桥接。
- SDL3 audio 实实现（蜂鸣器 stub）。
- I2S 音频硬件。
- `sdl_ttf`/`sdl_image`（先用内置字体）。
- 硬件改动（本项目针对原厂硬件，README 已声明）。

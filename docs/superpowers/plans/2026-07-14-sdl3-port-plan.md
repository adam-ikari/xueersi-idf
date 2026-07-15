# SDL3 底座移植 + 全硬件自检分页 demo 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除 LVGL，把渲染/输入底座换成 SDL3，同时把所有硬件驱动从 `main.c` 解耦成可复用模块，并用 SDL3 复刻现有 dashboard 的 15 页全硬件自检分页 UI。

**Architecture:** 五阶段顺序交付：P1 把 `main/main.c` 的硬件逻辑抽进新建的 `components/hardware/` 组件（保留现 LVGL 临时验证不回归）→ P2 引入 `georgik/sdl` + clone `esp_bsp_sdl`，写 `esp_bsp_sdl_xiaomiao.c` board 适配，验证能薄接 `hw_display` 的 `esp_lcd_panel_io` → P3 最小 SDL3 demo（一屏 + 一按键）→ P4 用 SDL3 复刻 15 页全硬件 demo 并删 LVGL → P5 收尾（配置、CLAUDE.md、merge-bin）。

**Tech Stack:** ESP-IDF v5.5.4（target `esp32`），SDL3 via `georgik/sdl` 组件 + `georgik/esp-idf-component-SDL_bsp`（git clone），ST7735 over SPI2 @60MHz，FreeRTOS，RGB565 byte-swapped。

## Global Constraints

- 本仓库**无单元测试框架**。验证 = `idf.py build` 编译通过 + `idf.py -p /dev/ttyACM0 flash monitor` 烧录后读串口日志 / 观察屏幕。每个任务结尾的"验证"步骤都遵循这个现实。
- ESP-IDF 激活方式：`source /home/gem/esp/esp-idf/export.sh`（**不是** `/home/gem/.venv`，那只有 esptool）。所有 `idf.py` 命令前都要先 source 它。
- 串口默认 `/dev/ttyACM0`，波特率 `460800`。
- 硬件层与 SDL3 渲染在**同一个 `demo_task`** 单线程顺序执行，本次**不加锁**。
- README 的硬件 spec / 引脚表 / GD32 协议**不动**，仍是真相源。
- `#define` 常量（引脚、寄存器、I2C 地址）从 `main.c` 迁到对应 `hw_*.h`。
- 不引 `sdl_ttf`/`sdl_image`，文本先用 SDL3 内置最小字体。
- 蜂鸣器 stub 掉 SDL3 audio（`SDL_Init` 不带 `SDL_INIT_AUDIO`）。
- 每个任务结束 commit（遵循 repo 的 commit 习惯，末尾加 `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`）。

---

## File Structure（锁定）

**新建：**
- `components/hardware/CMakeLists.txt`
- `components/hardware/include/hw_board.h`、`hw_display.h`、`hw_input.h`、`hw_i2c.h`、`hw_gd32.h`、`hw_mpu.h`、`hw_adc.h`、`hw_buzzer.h`、`hw_extio.h`、`hw_sd.h`
- `components/hardware/hw_board.c`、`hw_display.c`、`hw_input.c`、`hw_i2c.c`、`hw_gd32.c`、`hw_mpu.c`、`hw_adc.c`、`hw_buzzer.c`、`hw_extio.c`、`hw_sd.c`
- `main/sdl_demo.h`、`main/sdl_demo.c`
- `main/sdl_font.h`、`main/sdl_font.c`
- `components/esp_bsp_sdl/`（git clone，内含本次新增 `src/boards/esp_bsp_sdl_xiaomiao.c` + 改 `Kconfig`、`CMakeLists.txt`）

**修改：**
- `main/CMakeLists.txt`（去 `lvgl`，加 `sdl` + `esp_bsp_sdl` + `hardware`，SRCS 换 `sdl_demo.c`）
- `main/idf_component.yml`（去 `lvgl/lvgl`，加 `georgik/sdl`）
- `main/main.c`（瘦化为 `app_main`：`hw_board_init` → `sdl_demo_create` → `demo_task`，删除所有 LVGL 与内联硬件代码）
- `sdkconfig.defaults`（删 `CONFIG_LV_*`、`CONFIG_LV_CONF_SKIP`；保留 `SPI_MASTER_IN_IRAM`；加 SDL3 Kconfig）
- `dependencies.lock`（`idf.py reconfigure` 重生成）
- `CLAUDE.md`（架构描述更新）

**不变：** `README.md`、`go.py`、`GD32_firmware/`、根 `CMakeLists.txt`。

---

# Phase P1 — 硬件层解耦

目标：把 `main/main.c` 里的硬件逻辑抽进 `components/hardware/`，`main.c` 临时仍用 LVGL 调它们，确保硬件行为零回归。每个 `hw_*.c` 从 `main.c` 整段搬迁 + 改为非 static（对外暴露）+ 头文件声明接口。

## Task 1: 建 hardware 组件骨架 + CMakeLists

**Files:**
- Create: `components/hardware/CMakeLists.txt`
- Create: `components/hardware/include/`（空目录，后续任务填）

**Interfaces:**
- Produces: 一个空的 `hardware` 组件（`idf_component_register` 占位，无源文件），让后续任务逐个往里加文件即可编译。

- [ ] **Step 1: 写 CMakeLists.txt**

`components/hardware/CMakeLists.txt`：
```cmake
idf_component_register(SRCS
                       hw_board.c
                       hw_display.c
                       hw_input.c
                       hw_i2c.c
                       hw_gd32.c
                       hw_mpu.c
                       hw_adc.c
                       hw_buzzer.c
                       hw_extio.c
                       hw_sd.c
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_lcd esp_timer esp_driver_gpio esp_driver_spi
                                     esp_driver_i2c esp_driver_ledc esp_driver_sdspi
                                     esp_adc esp_hw_support heap spi_flash fatfs sdmmc)
```
注：先写全 10 个源文件名；这些文件在后续任务才真正创建。本任务先建目录 + 这个文件，**不**加入 `main/CMakeLists.txt` 依赖（避免引用不存在的源）。

- [ ] **Step 2: 验证目录结构**

Run: `ls components/hardware/ components/hardware/include/`
Expected: 两目录存在，`CMakeLists.txt` 在内。

- [ ] **Step 3: Commit**

```bash
git add components/hardware/CMakeLists.txt
git commit -m "scaffold: add components/hardware component skeleton"
```

---

## Task 2: hw_board.h — 状态 struct + 顶层接口

**Files:**
- Create: `components/hardware/include/hw_board.h`

**Interfaces:**
- Produces: `hw_board_state_t`（= 现 `main.c` 的 `board_state_t`）、`hw_board_init()`、`hw_board_update()`、`hw_board_process_timers()`、`hw_board_state()`（返回只读状态指针）。

- [ ] **Step 1: 写头文件**

把 `main/main.c:214-271` 的 `board_state_t` typedef 整段复制到 `hw_board.h`，重命名为 `hw_board_state_t`。再加顶层 API：
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// （这里放从 main.c 复制的 board_state_t 字段，结构名改为 hw_board_state_t）

void hw_board_init(void);                 // 调所有 hw_*_init（含 hw_display）
void hw_board_update(void);               // = 现 hardware_update(): 轮询传感器+I2C，填 hw_board_state_t
void hw_board_process_timers(void);       // = 现 hardware_process_timers(): buzzer 定时停等
const hw_board_state_t *hw_board_state(void);  // UI 只读访问
```

- [ ] **Step 2: Commit**

```bash
git add components/hardware/include/hw_board.h
git commit -m "hardware: add hw_board.h state struct + top-level API"
```

（此任务只声明接口；`hw_board.c` 实现在 P1.9，等所有子模块就位。）

---

## Task 3: hw_i2c — I2C 总线 + 读写原语

**Files:**
- Create: `components/hardware/include/hw_i2c.h`
- Create: `components/hardware/hw_i2c.c`
- Source (从 `main/main.c` 搬迁): `i2c_init` (907-951)、`i2c_write`/`i2c_write_reg`/`i2c_read_reg` (392-412)、全局 `s_i2c_bus`、`s_gd32_dev`、`s_mpu_dev` (315-317)、`PIN_NUM_I2C_*`、`I2C_*` 常量。

**Interfaces:**
- Produces: `hw_i2c_init()`、`i2c_master_bus_handle_t hw_i2c_bus()`、`i2c_master_dev_handle_t hw_i2c_gd32_dev()`、`hw_i2c_mpu_dev()`、`hw_i2c_write/write_reg/read_reg()`。

- [ ] **Step 1: 写 hw_i2c.h**

把 `PIN_NUM_I2C_SCL/SDA`、`I2C_TIMEOUT_MS`、`I2C_FREQ_HZ`、`GD32_ADDR`、`MPU6050_ADDR` 常量放进来（对外共享）。声明：
```c
#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#define PIN_NUM_I2C_SCL   GPIO_NUM_15
#define PIN_NUM_I2C_SDA   GPIO_NUM_21
#define I2C_FREQ_HZ       100000
#define I2C_TIMEOUT_MS    30
#define GD32_ADDR         0x40
#define MPU6050_ADDR      0x68

void hw_i2c_init(void);
i2c_master_bus_handle_t hw_i2c_bus(void);
i2c_master_dev_handle_t hw_i2c_gd32_dev(void);
i2c_master_dev_handle_t hw_i2c_mpu_dev(void);
esp_err_t hw_i2c_write(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len);
esp_err_t hw_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value);
esp_err_t hw_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len);
```

- [ ] **Step 2: 写 hw_i2c.c**

把 `main.c` 的 `s_i2c_bus`/`s_gd32_dev`/`s_mpu_dev` 全局搬来（改为本文件 static）。`i2c_init` 搬来改名为 `hw_i2c_init`，其中建 bus 后用 `i2c_master_bus_add_device` 建 `s_gd32_dev`(addr=GD32_ADDR) 和 `s_mpu_dev`(addr=MPU6050_ADDR)（原代码若在别处建 device，按原样搬）。`i2c_write/write_reg/read_reg` 搬来改名加 `hw_` 前缀，去掉 `static`。加 3 个 getter 返回 handle。

- [ ] **Step 3: 验证编译（暂时孤立）**

`hw_i2c.c` 此刻还没被谁引用，先确认它能单独编过。临时在 `components/hardware/CMakeLists.txt` 的 SRCS 只留 `hw_i2c.c`，跑：
Run: `source /home/gem/esp/esp-idf/export.sh && idf.py build`
Expected: 仍成功（main.c 没变，hw_i2c 编为独立对象）。若报 `hw_i2c.c` 引用了 main.c 还没迁移的符号，把缺失符号所属的常量也搬进 `hw_i2c.h`。
验证后**还原** CMakeLists 为完整 10 文件列表（编译会因其余 .c 不存在而失败，没关系——下一步 P1.3+ 会补齐；本步只验证 hw_i2c 自身语法，可临时只编它）。

- [ ] **Step 4: Commit**

```bash
git add components/hardware/include/hw_i2c.h components/hardware/hw_i2c.c
git commit -m "hardware: decouple hw_i2c (bus + read/write primitives)"
```

---

## Task 4: hw_display — SPI2/ST7735 显示 + flush 暴露

**Files:**
- Create: `components/hardware/include/hw_display.h`
- Create: `components/hardware/hw_display.c`
- Source: `lcd_init` (1207-1240)、`st7735_*` (1110-1195)、`lcd_display_on` (1196-1205)、`lvgl_flush_cb` 的 SPI 部分 (1011-1032)、`lcd_flush_ready_cb` (998-1009)、全局 `s_lcd_io_handle`、`s_lcd_display_on`、`s_lcd_first_flush_done` (314,326-327)、所有 `PIN_NUM_LCD_*`、`LCD_*`、`ST7735_*`、`MADCTL_*` 常量。

**Interfaces:**
- Produces: `hw_display_init()` → 返回/设置 `esp_lcd_panel_io_handle_t`；`hw_display_io()` 取句柄；`hw_display_flush(x1,y1,x2,y2,px_map)`（= 原 `lvgl_flush_cb` 的 CASET/RASET/RAMWR 部分）；`hw_display_on()`；`hw_display_first_flush_done()` / `hw_display_set_flush_ready_cb(cb, ctx)`（给上层注册 trans-complete 回调）。

- [ ] **Step 1: 写 hw_display.h**

常量全部搬入（`PIN_NUM_LCD_*`、`LCD_*`、`ST7735_*`、`MADCTL_*`）。声明：
```c
#pragma once
#include "esp_lcd_panel_io.h"
#include <stdint.h>
#include <stdbool.h>

// LCD pin/timing 常量（从 main.c 搬）
// ST7735 寄存器常量（从 main.c 搬）

esp_err_t hw_display_init(void);                       // = lcd_init: 建 SPI2 bus + panel io + st7735 init
esp_lcd_panel_io_handle_t hw_display_io(void);
void hw_display_flush(int x1, int y1, int x2, int y2, const uint8_t *px_map);  // CASET/RASET/RAMWR
void hw_display_on(void);
bool hw_display_first_flush_done(void);
typedef bool (*hw_display_flush_ready_cb_t)(void *ctx);  // = lcd_flush_ready_cb 签名
void hw_display_set_flush_ready_cb(hw_display_flush_ready_cb_t cb, void *ctx);
```

- [ ] **Step 2: 写 hw_display.c**

搬 `s_lcd_io_handle`/`s_lcd_display_on`/`s_lcd_first_flush_done` 为本文件 static。`lcd_init` → `hw_display_init`（返回 ESP_OK 而非 handle；handle通过 getter 取）。`st7735_*`/`lcd_display_on` 搬来去 static。`lvgl_flush_cb` 的 SPI 段抽成 `hw_display_flush`（去掉 LVGL 依赖，参数改为坐标+像素指针）。`lcd_flush_ready_cb` 存为可注册回调：`hw_display_set_flush_ready_cb` 存到 static，`esp_lcd_panel_io_register_event_callbacks` 在 `hw_display_init` 里注册一个内部 ISR thunk 调用该回调——**注：原代码在 app_main 里注册回调并传 LVGL display 作 ctx，迁移后由 SDL3 BSP 层注册，见 P2**。本任务先把 thunk 机制建好，回调可为 NULL。

- [ ] **Step 3: 编译验证（同 P1.2 方法，临时只编 hw_display.c + hw_i2c.c）**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py build`
Expected: 成功。

- [ ] **Step 4: Commit**

```bash
git add components/hardware/include/hw_display.h components/hardware/hw_display.c
git commit -m "hardware: decouple hw_display (SPI2/ST7735 + flush)"
```

---

## Task 5: hw_input — 6 GPIO 按键

**Files:**
- Create: `components/hardware/include/hw_input.h`
- Create: `components/hardware/hw_input.c`
- Source: `board_button_t` typedef (214-218)、`s_buttons[]` (293-300)、`buttons_init` (1079-1109)、`keypad_read_cb` (1040-1078)、`BUTTON_*` 常量。

**Interfaces:**
- Produces: `hw_button_t`(= `board_button_t`)、`hw_input_init()`、`const hw_button_t* hw_input_buttons(size_t *count)`、`bool hw_input_is_pressed(size_t idx)`（去抖后电平）、`hw_input_poll()`（P4 由 SDL3 demo 轮询，转 SDL 事件）。

- [ ] **Step 1: 写 hw_input.h**

搬 `BUTTON_ACTIVE_LEVEL`、`BUTTON_DEBOUNCE_MS`、`board_button_t`→`hw_button_t`。声明 init + buttons 数组 getter + is_pressed + poll。

- [ ] **Step 2: 写 hw_input.c**

搬 `s_buttons`（去 static 改 const 全局或 getter）。`buttons_init` → `hw_input_init`。`keypad_read_cb` 的去抖逻辑保留，但去掉 LVGL 依赖，核心抽成 `hw_input_is_pressed(idx)`。`hw_input_poll` 本任务先留空壳（P4 实现转 SDL 事件）。

- [ ] **Step 3: 编译验证**

- [ ] **Step 4: Commit**

```bash
git add components/hardware/include/hw_input.h components/hardware/hw_input.c
git commit -m "hardware: decouple hw_input (6 GPIO buttons)"
```

---

## Task 6: hw_gd32 — LED + 电机协议

**Files:**
- Create: `components/hardware/include/hw_gd32.h`
- Create: `components/hardware/hw_gd32.c`
- Source: `gd32_*` 全族 (414-480, 702-724)、`GD32_*_REG` 常量 (156-159)。依赖 `hw_i2c`。

**Interfaces:**
- Produces: `hw_gd32_probe()`、`hw_gd32_set_led(uint8_t idx, bool on)`、`hw_gd32_motor_set(uint8_t motor, bool dir, uint8_t speed)`、`hw_gd32_motor_stop_all()`、`hw_gd32_present()`。状态写入 `hw_board_state_t`（present, led1_on/led2_on, motor_running/dir/speed, last_gd32_err）。

- [ ] **Step 1: 写 hw_gd32.h**

搬 `GD32_LED1_REG`/`GD32_LED2_REG`/`GD32_MOTOR1_REG`/`GD32_MOTOR2_REG`。声明上述 5 个函数。访问 `hw_board_state_t` 需 `#include "hw_board.h"`。

- [ ] **Step 2: 写 hw_gd32.c**

搬 `gd32_mark_absent`/`gd32_write_reg`/`gd32_motor_stop_all`/`gd32_motor_set`，去 static 改名加 `hw_`。`gd32_probe` → `hw_gd32_probe`，用 `hw_i2c_bus()` 取 bus、`hw_i2c_gd32_dev()` 取 dev。写状态时调 `hw_board_state()`（P1.9 实现的 non-const getter——见下）改 `s_board` 字段。**状态写入**：为允许子模块写状态，`hw_board.h` 另提供 `hw_board_state_t *hw_board_state_mut(void)`（仅 hardware 组件内部用），P1.1 头文件补此声明。

- [ ] **Step 3: 编译验证**

- [ ] **Step 4: Commit**

```bash
git add components/hardware/include/hw_gd32.h components/hardware/hw_gd32.c components/hardware/include/hw_board.h
git commit -m "hardware: decouple hw_gd32 (LED + motor I2C protocol)"
```
（此 commit 顺带在 `hw_board.h` 补 `hw_board_state_mut` 声明，因 P1.1 未含。）

---

## Task 7: hw_mpu — MPU6050

**Files:**
- Create: `components/hardware/include/hw_mpu.h`
- Create: `components/hardware/hw_mpu.c`
- Source: `mpu_probe_and_init` (520-568)、`mpu_read` (569-614)、`MPU6050_*` 常量 (162-166)、`i16_be` (373-377)。依赖 `hw_i2c`、`hw_board`。

**Interfaces:**
- Produces: `hw_mpu_probe(force)`、`hw_mpu_read()`（填 `hw_board_state_t` 的 acc/gyro/pitch/roll/gesture/mpu_present/mpu_whoami/last_mpu_err）。

- [ ] **Step 1: 写 hw_mpu.h + .c**

搬常量、`i16_be`、两函数，去 static 改名。`hw_mpu_read` 计算 pitch/roll 的数学（原在 mpu_read 内）原样搬。

- [ ] **Step 2: 编译验证**

- [ ] **Step 3: Commit**

```bash
git add components/hardware/include/hw_mpu.h components/hardware/hw_mpu.c
git commit -m "hardware: decouple hw_mpu (MPU6050)"
```

---

## Task 8: hw_adc — 光照/热敏/扩展 ADC

**Files:**
- Create: `components/hardware/include/hw_adc.h`
- Create: `components/hardware/hw_adc.c`
- Source: `adc_init` (820-855)、`adc_read_one` (615-623)、`adc_read_sensors` (624-682)、`pct_from_raw` (338-342)、`ADC_*` 常量 (168-172)、`s_adc_handle` (313)。依赖 `hw_board`、`hw_input`（ADC_EXT_IN 通道映射 GPIO32/33，需确认 channel——原代码已有映射）。

**Interfaces:**
- Produces: `hw_adc_init()`、`hw_adc_read_light()`→int raw、`hw_adc_read_temp()`→raw、`hw_adc_read_ext(idx)`→raw、`hw_adc_update()`（= `adc_read_sensors`，填 state.light_raw/temp_raw/ext_raw/last_adc_err）。

- [ ] **Step 1: 写 hw_adc.h + .c**

搬 `s_adc_handle`、常量、`pct_from_raw`、三函数。`hw_adc_read_*` 返回 raw，UI 端做百分比转换（或保留 `pct_from_raw` 暴露）。

- [ ] **Step 2: 编译验证**

- [ ] **Step 3: Commit**

```bash
git add components/hardware/include/hw_adc.h components/hardware/hw_adc.c
git commit -m "hardware: decouple hw_adc (light/therm/ext ADC)"
```

---

## Task 9: hw_buzzer + hw_extio + hw_sd

**Files:**
- Create: `components/hardware/include/hw_buzzer.h`、`hw_extio.h`、`hw_sd.h`
- Create: `components/hardware/hw_buzzer.c`、`hw_extio.c`、`hw_sd.c`
- Source: `buzzer_*` (481-510, 952-985)、`BUZZER_*`/`EXT_*` 常量 (174-182)、`s_buzzer_*` (319-320)；`ext_io_init` (856-906)、`ext_output_set` (683-700)、`ext_pwm_*`（在 ext_io_init 内）、`PIN_NUM_EXT_*` (127-130)；`sd_try_mount` (753-792)、`sd_unmount` (793-819)、`SD_SPI_MAX_FREQ_KHZ` (153)、`PIN_NUM_SD_CS` (123)、`s_sd_card` (318)。

**Interfaces:**
- Produces（buzzer）: `hw_buzzer_init()`、`hw_buzzer_beep(freq,ms)`、`hw_buzzer_stop()`、`hw_buzzer_timer()`（并入 process_timers 的定时停，原 `hardware_process_timers` 的 buzzer 段）。
- Produces（extio）: `hw_extio_init()`、`hw_extio_set(idx,bool)`、`hw_extio_set_pwm(idx,duty)`。
- Produces（sd）: `hw_sd_try_mount()`、`hw_sd_unmount()`、`hw_sd_info(char*name,size_t,uint32_t*mb)`。

- [ ] **Step 1: 写 3 个 .h + 3 个 .c**

逐一搬迁。`hw_buzzer_timer` 从 `hardware_process_timers` 抽出 buzzer 段。

- [ ] **Step 2: 编译验证**

- [ ] **Step 3: Commit**

```bash
git add components/hardware/include/hw_buzzer.h components/hardware/include/hw_extio.h components/hardware/include/hw_sd.h components/hardware/hw_buzzer.c components/hardware/hw_extio.c components/hardware/hw_sd.c
git commit -m "hardware: decouple hw_buzzer, hw_extio, hw_sd"
```

---

## Task 10: hw_board.c — 顶层 init/update/process_timers + 状态全局

**Files:**
- Create: `components/hardware/hw_board.c`
- Source: `s_board` (304-311)、`hardware_init` (986-997)、`hardware_update` (745-752)、`hardware_process_timers` (511-519)、`i2c_probe_devices` (726-744)、`gd32_probe`(702-724，已搬入 hw_gd32)、`s_last_*_probe_ms`/`s_*_probe_seen` (322-325)。

**Interfaces:**
- Consumes: 所有 `hw_*_init`/`hw_*_probe`/`hw_*_read`/`hw_buzzer_timer`。
- Produces: `hw_board_init()` 调 `hw_i2c_init`→`hw_display_init`→`hw_input_init`→`hw_adc_init`→`hw_buzzer_init`→`hw_extio_init`→（SD 按需）+ 初次 probe；`hw_board_update()` 调 `i2c_probe_devices`(→gd32/mpu probe) + `hw_adc_update` + `hw_mpu_read` + `hw_sd`(惰性)；`hw_board_process_timers()` 调 `hw_buzzer_timer`；`hw_board_state()`/`hw_board_state_mut()` 返回 `s_board`。

- [ ] **Step 1: 写 hw_board.c**

搬 `s_board` 为本文件 static（含初始值）。`hardware_init`→`hw_board_init`，调各子模块 init。`hardware_update`→`hw_board_update`，调 `i2c_probe_devices`(内部调 gd32/mpu probe) + adc_update + mpu_read。`hardware_process_timers`→`hw_board_process_timers`。两个 getter（const 给 UI，mut 给内部子模块）。

- [ ] **Step 2: 编译验证（hardware 组件完整自洽）**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py build`
Expected: hardware 组件 10 文件全编译通过（此刻 main.c 还没引用 hardware 组件，所以仍编旧 main.c + lvgl）。

- [ ] **Step 3: Commit**

```bash
git add components/hardware/hw_board.c
git commit -m "hardware: implement hw_board top-level init/update/process_timers"
```

---

## Task 11: main.c 改用 hardware 组件（LVGL 仍保留，验证零回归）

**Files:**
- Modify: `main/CMakeLists.txt`（加 `REQUIRES hardware`）
- Modify: `main/main.c`（删除已搬迁的硬件函数与全局，改为调用 `hw_*`）

**Interfaces:**
- Produces: 仍跑 LVGL dashboard，但硬件层全部走 `components/hardware`。这是回归验证基线。

- [ ] **Step 1: 改 main/CMakeLists.txt**

在 `idf_component_register` 的 `PRIV_REQUIRES` 加 `hardware`，并从 SRCS 仍保留 `main.c`（LVGL 相关 REQUIRES 暂留）。

- [ ] **Step 2: 改 main.c**

删除所有已搬到 hardware 的硬件函数/全局/常量（保留 LVGL UI 函数与 `ui_*`、`lvgl_*`、`s_ui`、history 数组）。把对硬件的调用全部改成 `hw_*` 调用，例：`hardware_init()`→`hw_board_init()`、`hardware_update()`→`hw_board_update()`、`hardware_process_timers()`→`hw_board_process_timers()`、`adc_read_sensors`→隐于 `hw_board_update`、UI 读 `s_board`→读 `hw_board_state()`。`lvgl_flush_cb` 改调 `hw_display_flush`；flush ready cb 注册改调 `hw_display_set_flush_ready_cb`（或直接在 `hw_display` 内注册一个调 `lv_display_flush_ready` 的 thunk——但那会把 LVGL 耦回 hardware；**更干净**：main.c 侧用 `hw_display_set_flush_ready_cb` 注册一个 main.c 的回调调 `lv_display_flush_ready`，hardware 组件不碰 LVGL）。

- [ ] **Step 3: 编译 + 烧录验证（回归基线）**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash monitor`
Expected: 编译通过；烧录后屏幕正常显示 dashboard，所有页功能与解耦前一致（翻页、LED/电机/蜂鸣器/ADC/MPU/SD 正常）。串口无新 error。

- [ ] **Step 4: Commit**

```bash
git add main/CMakeLists.txt main/main.c
git commit -m "refactor: main.c uses components/hardware (LVGL retained, behavior unchanged)"
```

**这是 P1 的关键回归门。** 若任何硬件行为变化，回退定位到本任务。

---

# Phase P2 — 引入 SDL3 + BSP board 适配 + 验证风险点

## Task 12: 加 georgik/sdl 组件依赖

**Files:**
- Modify: `main/idf_component.yml`

- [ ] **Step 1: 改 yml**

```yaml
dependencies:
  georgik/sdl: "*"
```
（暂保留 `lvgl/lvgl: "9.5.0"` 直到 P4 删 LVGL，避免中途断编译。）

- [ ] **Step 2: reconfigure 拉组件**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py reconfigure`
Expected: 拉取 `georgik/sdl` 到 `managed_components/`，`dependencies.lock` 更新。

- [ ] **Step 3: 编译验证**

Run: `idf.py build`
Expected: 成功（SDL3 主体编入，但还没人用它）。

- [ ] **Step 4: Commit**

```bash
git add main/idf_component.yml dependencies.lock
git commit -m "deps: add georgik/sdl component"
```

---

## Task 13: clone esp_bsp_sdl + 验证 board.c 结构（风险点）

**Files:**
- Create: `components/esp_bsp_sdl/`（git clone）

**这是设计 §2 标注的最大不确定点。** 本任务目的：搞清 `esp_bsp_sdl` 的 board `.c` 到底是直接调 `esp_lcd` 还是绕 ESP-BSP 板级抽象，决定后续适配写法。

- [ ] **Step 1: clone 仓库**

Run: `git clone https://github.com/georgik/esp-idf-component-SDL_bsp components/esp_bsp_sdl`
Expected: clone 成功。

- [ ] **Step 2: 读最接近的 board 源码（M5 Atom S3，128×128 SPI）**

Read: `components/esp_bsp_sdl/src/boards/esp_bsp_sdl_m5_atom_s3.c`（或仓库实际文件名）
重点确认：
- 它是否 `#include` 了重的 ESP-BSP 板级组件（`esp_bsp_*`），还是直接用 `esp_lcd_panel_io_*`？
- `esp_bsp_sdl_init` 的签名与如何把 `esp_lcd_panel_io_handle_t` 喂给 SDL3 video backend？
- 像素 push 在哪：`esp_lcd_panel_io_tx_color` 还是 `esp_lcd_panel_draw_bitmap`？

- [ ] **Step 3: 据结构决定适配策略**

- 若 board `.c` 直接调 `esp_lcd`：仿照它写 `esp_bsp_sdl_xiaomiao.c`，调 `hw_display_init()`/`hw_display_flush`。
- 若绕 ESP-BSP 重抽象：**fork 思路**——写一个不依赖 ESP-BSP 的最小 board `.c`，直接实现 `esp_bsp_sdl` 的 video backend 接口（把 SDL3 的 framebuffer flush 调 `hw_display_flush`）。可能需读 SDL3 那侧的 video driver hook 签名（在 `managed_components/georgik__sdl` 里）。
- 记录决定到本任务 commit message。

- [ ] **Step 4: Commit（含决定说明）**

```bash
git add components/esp_bsp_sdl
git commit -m "deps: vendor esp_bsp_sdl; board.c strategy = <直接 esp_lcd / 最小自写>"
```

---

## Task 14: 写 esp_bsp_sdl_xiaomiao.c board 适配

**Files:**
- Create: `components/esp_bsp_sdl/src/boards/esp_bsp_sdl_xiaomiao.c`
- Modify: `components/esp_bsp_sdl/Kconfig`（加 `CONFIG_ESP_BSP_SDL_BOARD_XIAOMIAO`）
- Modify: `components/esp_bsp_sdl/CMakeLists.txt`（加 `elseif(CONFIG_ESP_BSP_SDL_BOARD_XIAOMIAO)` 编入新文件）

**Interfaces:**
- Consumes: `hw_display`（`hw_display_init`/`hw_display_io`/`hw_display_flush`/`hw_display_on`/`hw_display_set_flush_ready_cb`）。
- Produces: `esp_bsp_sdl_init()`（或 SDL3 期望的 init），让 SDL3 的 video backend 渲染结果推到 ST7735。

- [ ] **Step 1: 改 Kconfig 加板选项**

仿仓库既有 board 的 Kconfig entry，加：
```
config ESP_BSP_SDL_BOARD_XIAOMIAO
    bool "Xiaomiao handheld (ESP32-WROVER-B + ST7735 160x128)"
    select ...（按既有 board 模式）
```

- [ ] **Step 2: 改 CMakeLists.txt 加 elseif 分支**

仿既有 board 的 `elseif(CONFIG_ESP_BSP_SDL_BOARD_...)` 块，加 xiaomiao。

- [ ] **Step 3: 写 esp_bsp_sdl_xiaomiao.c**

按 P2.3 Step3 的策略：
- video backend：调 `hw_display_init()`（若 `hw_board_init` 已调过则不重复——注意 idempotent，`hw_display` 内 `if (s_lcd_io_handle) return`），SDL3 渲染回调里调 `hw_display_flush(x1,y1,x2,y2,px_map)`。
- **颜色字节序**：屏是 RGB565 byte-swapped。若 SDL3 出标准 RGB565，在 flush 前 swap 字节（或让 SDL3 配 `SDL_PIXELFORMAT_RGB565` 后 BSP 端 `__builtin_bswap16` 整 buffer）。实现期定具体做法，二选一。
- 分辨率：160×128，横屏（对应原 ST7735 rot90）。
- input：`hw_input_poll` 转 SDL3 事件（本任务先占位，P4 实现完整；P3 最小 demo 只需 video 通）。

- [ ] **Step 4: menuconfig 选板 + 编译**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py menuconfig` → ESP-BSP SDL Configuration → 选 Xiaomiao。
Run: `idf.py build`
Expected: 成功。

- [ ] **Step 5: Commit**

```bash
git add components/esp_bsp_sdl/src/boards/esp_bsp_sdl_xiaomiao.c components/esp_bsp_sdl/Kconfig components/esp_bsp_sdl/CMakeLists.txt
git commit -m "bsp: add esp_bsp_sdl_xiaomiao board adapter (ST7735 via hw_display)"
```

---

# Phase P3 — 最小 SDL3 demo（一屏 + 一按键）

## Task 15: sdl_font.h/.c 最小字体封装

**Files:**
- Create: `main/sdl_font.h`、`main/sdl_font.c`

- [ ] **Step 1: 写 sdl_font.h**

声明一个用 SDL3 内置 8×8（`SDLTest_DrawString` 或 `SDL_RenderDebugText`，看 SDL3 版本提供哪个）的封装：
```c
#pragma once
#include <SDL3/SDL.h>
void sdl_font_init(SDL_Renderer *r);
void sdl_font_draw(SDL_Renderer *r, int x, int y, const char *text, SDL_Color c);
```

- [ ] **Step 2: 写 sdl_font.c**

薄封装 SDL3 调试文本 API。

- [ ] **Step 3: Commit**

```bash
git add main/sdl_font.h main/sdl_font.c
git commit -m "sdl: add minimal font wrapper (SDL3 built-in 8x8)"
```

---

## Task 16: sdl_demo.h/.c 骨架 + 最小 demo（一屏 + 一按键）

**Files:**
- Create: `main/sdl_demo.h`、`main/sdl_demo.c`
- Modify: `main/main.c`（`app_main` 调 `hw_board_init`→`sdl_demo_create`→`demo_task`；**暂时双轨**：用一个 menuconfig 开关 `CONFIG_XIAOMIAO_ENABLE_SDL` 切 SDL demo / LVGL dashboard，便于回退）。

**Interfaces:**
- Produces: `sdl_demo_create()`、`sdl_demo_task(void*)`。

- [ ] **Step 1: 加 menuconfig 开关**

在 `main/Kconfig.main`（新建，若不存在）加：
```
config XIAOMIAO_ENABLE_SDL
    bool "Use SDL3 demo instead of LVGL dashboard"
    default n
```
`sdkconfig.defaults` 暂不改（默认仍 LVGL）。

- [ ] **Step 2: 写 sdl_demo.h + .c 最小版**

```c
// sdl_demo.h
#pragma once
void sdl_demo_create(void);
void sdl_demo_task(void *arg);
```
`sdl_demo.c`：
- `sdl_demo_create`：`SDL_Init(SDL_INIT_VIDEO)`；`SDL_CreateWindow`（160×128，hidden/offscreen——ESP32 无窗口系统，用 SDL3 的 framebuffer/屏幕后端）；取 renderer/surface。
- `sdl_demo_task`：loop（同设计 §4）→ `hw_board_update` → `SDL_PollEvent`（P3 先不处理按键，或只打印）→ 渲染一屏（填充背景色 + 用 `sdl_font_draw` 写 "SDL3 OK" + 显示一个 ADC 值如光照）→ `SDL_RenderPresent`（BSP flush）→ usleep(16ms)。

- [ ] **Step 3: 改 main.c 双轨 app_main**

```c
void app_main(void){
    hw_board_init();
#if CONFIG_XIAOMIAO_ENABLE_SDL
    sdl_demo_create();
    xTaskCreate(sdl_demo_task, "sdl_demo", 10*1024, NULL, 5, NULL);
#else
    // 旧 LVGL dashboard 启动路径（lvgl_task）原样保留
    ...
#endif
}
```

- [ ] **Step 4: 启用 SDL 编译 + 烧录验证**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py menuconfig` → 开 `XIAOMIAO_ENABLE_SDL`。
Run: `idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash monitor`
Expected: 屏幕亮（首帧 flush 后），显示 "SDL3 OK" + 光照数值在变，串口见 SDL3 init 日志无崩溃。**这证明 video backend 通。**

- [ ] **Step 5: Commit**

```bash
git add main/sdl_demo.h main/sdl_demo.c main/main.c main/Kconfig.main
git commit -m "sdl: minimal demo (one screen + live ADC), build switch to LVGL"
```

**这是 P3 验证门：SDL3 真能渲染到屏。**

---

# Phase P4 — 全硬件分页 demo + 删 LVGL

P4 按"输入→各页→删LVGL"分任务。每页实现 = 渲染 + 事件处理（A/B）+ 调对应 `hw_*`。

## Task 17: 输入集成（6 按键 → SDL3 事件）

**Files:**
- Modify: `main/sdl_demo.c`
- Modify: `components/hardware/hw_input.c`（`hw_input_poll` 实现）

- [ ] **Step 1: hw_input_poll 转 SDL 事件**

`hw_input_poll`：遍历 6 按钮，去抖后边沿检测（按下/松开），调 `SDL_PushEvent` 投 `SDL_EVENT_KEY_DOWN`/`UP`，scancode 映射：UP→`SDL_SCANCODE_UP`，DOWN→`SDL_SCANCODE_DOWN`，LEFT→`SDL_SCANCODE_LEFT`，RIGHT→`SDL_SCANCODE_RIGHT`，A→`SDL_SCANCODE_RETURN`，B→`SDL_SCANCODE_ESCAPE`。需在 sdl_demo_task 的 poll 前调 `hw_input_poll()`。

- [ ] **Step 2: sdl_demo 事件路由骨架**

`sdl_demo_task` 里 `while(SDL_PollEvent(&e))`：方向键翻页、Return→action、Escape→cancel/adjust。先只实现翻页（左右），其余每页 handler 留函数指针表（P4.2+ 填）。

- [ ] **Step 3: 烧录验证**

Expected: 左右键能翻页（此时所有页都是占位背景 + 页名文本）。

- [ ] **Step 4: Commit**

```bash
git add main/sdl_demo.c components/hardware/hw_input.c
git commit -m "sdl: wire 6 buttons to SDL3 events + page nav skeleton"
```

---

## Task 18: LIGHT / THERM 页（折线图 + 历史）

**Files:**
- Modify: `main/sdl_demo.c`（加页渲染 + history 数组——history 从 main.c 搬到 sdl_demo.c 或新 `sdl_demo_history.c`）

- [ ] **Step 1: 搬 history 数组与 push 逻辑**

把 `s_light_history`/`s_therm_history`/`sensor_history_push*` 从 main.c（若 P1 已随 UI 留下）搬到 sdl_demo 侧。

- [ ] **Step 2: 实现折线绘制**

用 `SDL_RenderLines` 在 160×128 上画折线，min/max 映射沿用 `THERM_HISTORY_MIN/MAX_PCT`。

- [ ] **Step 3: 页渲染 + 刷新**

LIGHT/THERM 页读 `hw_board_state()->light_raw/temp_raw`，push 历史，画值 + 折线 + 进度条。

- [ ] **Step 4: 烧录验证**

Expected: LIGHT 页数值随光照变 + 折线滚动；THERM 页温度值 + 折线。

- [ ] **Step 5: Commit**

```bash
git add main/sdl_demo.c
git commit -m "sdl: LIGHT/THERM pages with line-chart history"
```

---

## Task 19: MOTION 页（MPU6050）

- [ ] **Step 1:** 读 `hw_board_state()` 的 acc/gyro/pitch/roll/gesture，绘制数值 + gesture 文本。
- [ ] **Step 2:** 烧录验证 MPU 在线时数值动，离线显示 ABSENT。
- [ ] **Step 3:** Commit `sdl: MOTION page (MPU6050)`.

---

## Task 20: LED1/LED2 + BUZZER 页

- [ ] **Step 1:** LED 页 A 键调 `hw_gd32_set_led` toggle；BUZZER 页 A 调 `hw_buzzer_beep`、B 调 `hw_buzzer_stop`。
- [ ] **Step 2:** 烧录验证 LED 点灭、蜂鸣器响。
- [ ] **Step 3:** Commit `sdl: LED1/LED2 + BUZZER pages`.

---

## Task 21: MOTOR1/MOTOR2 页

- [ ] **Step 1:** A 键 `hw_gd32_motor_set(motor, dir, speed)` 正/反/停切换，B 键调速（沿用 `ui_adjust` 的步进逻辑）。
- [ ] **Step 2:** 烧录验证电机转（**注意：测电机时按 README 建议拔电机或小心**）。
- [ ] **Step 3:** Commit `sdl: MOTOR1/MOTOR2 pages`.

---

## Task 22: SD + GPIO25/26 + ADC32/33 页

- [ ] **Step 1:** SD 页调 `hw_sd_info` 显示卡名容量，A 重挂载；GPIO25/26 页 A 调 `hw_extio_set`/`hw_extio_set_pwm`；ADC32/33 页读 `hw_adc_read_ext` 画百分比条。
- [ ] **Step 2:** 烧录验证 SD 挂载信息、扩展 IO 输出、ADC 数值。
- [ ] **Step 3:** Commit `sdl: SD/GPIO25/GPIO26/ADC32/ADC33 pages`.

---

## Task 23: SYSTEM + ABOUT 页 + 状态栏 + action 提示

- [ ] **Step 1:** SYSTEM 页显示 CPU/flash/PSRAM/IDF 版本/空闲堆（`heap_caps_get_free_size`）/各外设 last err；ABOUT 页可滚动静态文本。顶部状态栏（GD32/MPU/SD present），底部 action 提示（`set_action` 等价，~850ms）。
- [ ] **Step 2:** 烧录验证全部 15 页遍历 + 状态栏 + 提示。
- [ ] **Step 3:** Commit `sdl: SYSTEM/ABOUT pages + status bar + action hint`.

---

## Task 24: 删 LVGL + 设 SDL demo 为默认

**Files:**
- Modify: `main/CMakeLists.txt`（删 `lvgl` REQUIRES，SRCS 去 `main.c` 的 LVGL 代码——实际 main.c 已瘦化）
- Modify: `main/main.c`（删 `#if CONFIG_XIAOMIAO_ENABLE_SDL` 双轨，直接 SDL）
- Modify: `main/idf_component.yml`（删 `lvgl/lvgl`）
- Modify: `sdkconfig.defaults`（删 `CONFIG_LV_*`、`CONFIG_LV_CONF_SKIP`；删 `XIAOMIAO_ENABLE_SDL` 开关或固定为 y）
- Delete: `main/Kconfig.main`（若开关不再需要）

- [ ] **Step 1: 删 LVGL 依赖与配置**

`idf_component.yml` 删 `lvgl/lvgl`；`sdkconfig.defaults` 删所有 `CONFIG_LV_*` 行。`main.c` 删 LVGL include 与所有 `lv_*`/`ui_*` 残留（P1.10 后应已无 LVGL UI 代码，只剩 app_main 双轨——删双轨）。

- [ ] **Step 2: 删 managed_components/lvgl**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py reconfigure`
Expected: `managed_components/lvgl__lvgl` 移除。

- [ ] **Step 3: 全量编译 + 烧录验证**

Run: `idf.py build && idf.py -p /dev/ttyACM0 -b 460800 flash monitor`
Expected: 干净编译，烧录后 SDL3 全 15 页 demo 正常运行，无 LVGL 残留符号。

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: remove LVGL, SDL3 full-hardware demo is now the firmware"
```

**P4 完成门：LVGL 彻底移除，SDL3 demo 是唯一 UI。**

---

# Phase P5 — 收尾

## Task 25: sdkconfig.defaults 定稿 + CLAUDE.md 更新

**Files:**
- Modify: `sdkconfig.defaults`
- Modify: `CLAUDE.md`
- Delete: `sdkconfig`（gitignored，但确认无残留）

- [ ] **Step 1: sdkconfig.defaults 定稿**

确认 SDL3 相关 Kconfig 已含；`SPI_MASTER_IN_IRAM` 在；无 `CONFIG_LV_*`。

- [ ] **Step 2: 更新 CLAUDE.md**

改"ESP32 Firmware Architecture"段：LVGL→SDL3；新增"硬件层 `components/hardware/`"说明（10 模块 + `hw_board_state_t` + `hw_board_init/update/process_timers`）；改 runtime model：`lvgl_task`→`sdl_demo_task`；加"wasm3 规划"小节（指向设计 doc）。

- [ ] **Step 3: Commit**

```bash
git add sdkconfig.defaults CLAUDE.md
git commit -m "docs: finalize sdkconfig.defaults + update CLAUDE.md for SDL3/hardware layer"
```

---

## Task 26: 生成 merge-bin 固件

- [ ] **Step 1: 全量 build + merge**

Run: `source /home/gem/esp/esp-idf/export.sh && idf.py build && idf.py merge-bin`
Expected: `build/merged-binary.bin` 生成。

- [ ] **Step 2: （可选）改名对齐 README release 命名**

Run: `cp build/merged-binary.bin build/xiaomiao-merged.bin`（仅本地，不入 git——`*.bin` gitignored）。

- [ ] **Step 3: 烧录合并 bin 验证**

Run: `esptool.py --chip esp32 -b 460800 write_flash 0x0 build/xiaomiao-merged.bin`
Expected: 烧录后开机正常运行 SDL3 demo。

- [ ] **Step 4: Commit（无文件改动则跳过；或只更新日志）**

本任务无源码改动，固件产物 gitignored。若要记录里程碑：
```bash
git commit --allow-empty -m "milestone: SDL3 firmware, merged-bin verified on hardware"
```

---

## Self-Review（计划完成后自查）

**1. Spec 覆盖：** 逐项对照设计 doc §1-§6：
- §1 硬件层 10 模块 → P1.1-P1.9 ✓
- §2 SDL3 集成 + BSP + 风险点 → P2.1-P2.3（P2.2 即风险验证）✓
- §3 15 页 + 交互 + 渲染策略 → P3 + P4.2-P4.7 ✓（页数 15：LIGHT/THERM/MOTION/LED1/LED2/BUZZER/MOTOR1/MOTOR2/SD/GPIO25/GPIO26/ADC32/ADC33/SYSTEM/ABOUT）
- §4 单 worker 线程模型 → P3.2 sdl_demo_task ✓
- §5 文件结构 + 配置 → 各任务 Files 块 + P4.8/P5.1 ✓
- §6 验收 6 条 + 5 阶段 → P1-P5 对应 ✓

**2. 占位符扫描：** 计划中 `<直接 esp_lcd / 最小自写>` 是 P2.2 决策点（设计 doc 已标开放），非占位；其余无 TODO/TBD。

**3. 类型/命名一致：** `hw_board_state_t`、`hw_board_init/update/process_timers/state/state_mut`、`hw_display_flush(io/...)`、`hw_input_poll`、`hw_gd32_set_led/motor_set/motor_stop_all/probe`、`hw_mpu_probe/read`、`hw_adc_read_light/temp/ext/update`、`hw_buzzer_init/beep/stop/timer`、`hw_extio_init/set/set_pwm`、`hw_sd_try_mount/unmount/info`、`sdl_demo_create/task`、`sdl_font_init/draw` —— 全计划统一。

**4. 风险已在 P2.2 显式标注**（设计 doc §2 最大不确定点），有明确验证步骤。

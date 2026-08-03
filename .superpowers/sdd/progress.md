# TinyGL 项目进度 — 恢复后

**Branch**: tinygl-benchmark（自 GitHub adam-ikari/xueersi-idf 恢复，07-30 谱系 7f7b6a0）

- [x] 恢复项目 + 删模拟器（6525be5）
- [x] 沙漠天空盒 sky/sand/horizon + 128x128 修复分区溢出（7c53807, 560749b）
- [x] unlit 快速路径（修天空盒 RGB_MIX 溢出）+ 反射立方体（560749b）
- 待硬件验证：烧录后确认沙漠场景 + 反射立方体 + FPS
- [x] wasm GL 命令流架构 (2cd46cd)
  - wasm3 游戏用 gl* 调用提交整个场景 → glcmd_stream 编码 → core1 重放（single-slot latest-wins 丢帧）。
  - 修了恢复版崩溃：旧 wasm3 签名用 f64'F' 传 f32 参数 → missing imported function panic。现 22 个 gl* 宿主签名与 wasm 导入一致。
  - wasm_game.wat 画金属立方体(多纹理)+方块，纹理只传整数名。
  - 固件 955KB，合并 1.06MB。待硬件验证。
- [x] wasm 改 C++ 开发 + wasm 绘制沙漠天空盒 (867a7a0)
  - wasm_game.cpp (clang++ wasm32) 取代 WAT，可维护。全场景经 gl* 提交：沙漠天空盒(顶sky/底sand/侧horizon subdiv2)+多纹理金属立方体+小方块。
  - CMake 从 .cpp 构建 wasm；删 wasm_game.wat。
  - 删除原生 draw_skybox（wasm 唯一作者）。固件 958KB，合并 1.06MB。
  - 遗留死代码：原生 draw_metal_cube/draw_reflective_cube/draw_textured_cube（可清理）。
- [x] 修 wasm3_game 栈溢出（硬件验证 ✅ 运行正常）
  - 根因：xtensa GCC 无 tail-call 优化（`__has_attribute(musttail)`=0），wasm3 线程化解释器每执行 1 条 wasm 指令嵌套 ~20B 原生栈，仅当顶层 m3_CallV() 返回才回卷。game_update 被 -O3 全展开为 ~2200 ops（971 内联 + 6×206 skybox_face）→ 需 ~44KB；32KB 栈第三次溢出（778f219 曾 16→32KB，逐顶点反射 5658946 又 +555 ops）。
  - 修复：`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y` + wasm3_game 任务改用 128KB PSRAM 栈（`heap_caps_malloc(MALLOC_CAP_SPIRAM|8BIT)` + `xTaskCreateStaticPinnedToCore`）。`d_m3CascadedOpcodes` 只改 opcode 表索引，无法修 dispatch，不要走弯路。
  - 待办：**金属立方体没有金属效果**（反射/高光未显示，多纹理叠加未生效）。

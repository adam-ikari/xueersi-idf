# 用 WAMR 替换 wasm3 引擎 + 图形引擎 SRAM 优先内存设计

**日期**: 2026-08-03
**分支**: tinygl-benchmark
**状态**: 已批准（进行实现规划前）

## 目标

1. 用 **WAMR（wasm-micro-runtime）** 替换 **wasm3** 作为 wasm 游戏引擎，消除 wasm3 在 xtensa GCC 上无尾调用优化导致的每-opcode 原生栈增长问题（此前用 128KB PSRAM 栈救急）。
2. 渲染链路零改动：同一份 `wasm_game.wasm`、同一 GL 命令流桥（`glcmd_stream`）、core1 重放 TinyGL。
3. 设计零开销的跨核同步（N 缓冲 ping-pong + paced，可配 2/3，无 memcpy、无锁）。
4. 显示路径升级为 N 帧缓冲流水线（可配 2/3，默认 3），**保留一个过去帧**供时序后处理，paced 无丢帧。
5. 设计整机内存管理（`mem_mgr`）：**图形引擎 SRAM 优先**，WAMR 全进 PSRAM，纹理留 flash，wasm 无感知、可移植。

## 背景

- 当前 `wasm3_game_task` 用 wasm3 解释 `wasm_game.wasm`（导入 22 个 `env` GL 函数），编码进 `glcmd_stream`，core1 重放。
- wasm3 的线程化解释器在 xtensa GCC 下无 `musttail`，每执行 1 条 wasm 指令嵌套 ~20B 原生栈，`game_update` 全展开 ~2200 ops → 需 ~44KB 栈。已用 128KB PSRAM 栈临时修复（`f207503`）。
- WAMR 是**循环式解释器**（fast interp），原生栈有界，不需要该 hack。

## §1 引擎替换（WAMR）

**文件调整**
- `main/wasm3_game.c` → `main/wasm_game.c`（WAMR 实现），头文件 `wasm_game.h`（任务名 `"wasm_game"`）
- 删除旧的 `main/wasm_game.c`（未编译的 WAMR 示例，死代码）
- `main/CMakeLists.txt`：`wasm3_game.c` → `wasm_game.c`，`PRIV_REQUIRES` 去掉 `wasm3`
- 删除 `components/wasm3/`

**wasm 构建不变**：clang++ → wasm32（保持 `--initial-memory=131072 --max-memory=131072`），wasm 字节码嵌入方式不变。

**`wasm_game.c`（core0 任务，30fps 节奏与 wasm3 版一致）**
1. `wasm_runtime_init()`（一次）
2. 注册 22 个 GL 原生函数到 `"env"` 模块，签名严格匹配 wasm 导入（WAMR 签名格式）：
   - `"(i)"`: glBegin / glEnable / glDisable / glMatrixMode / glClear / glDepthMask / glActiveTexture
   - `"()"`: glEnd / glLoadIdentity / glPushMatrix / glPopMatrix / glFlush
   - `"(FFF)"`: glVertex3f / glColor3f / glNormal3f / glTranslatef
   - `"(FFFF)"`: glRotatef
   - `"(FF)"`: glTexCoord2f
   - `"(ii)"`: glBindTexture
   - `"(iii)"`: glTexEnvi
   - `"(iiFFFF)"`: glTexEnvfv
   - `"(iFF)"`: glTexOffset
   - 每个包装：`static void host_xxx(wasm_exec_env_t env, ...args)` → 复用现有 `glcmd_u8/u32/f32` 编码，渲染逻辑零改动
3. `wasm_runtime_load(wasm_game_wasm, len, &err)`
4. `wasm_runtime_instantiate(module, 0, heap, &err)`（默认栈 0，因为显式建 exec_env）
5. `wasm_runtime_create_exec_env(inst, stack_size)`
6. `wasm_runtime_lookup_function` + `wasm_runtime_call_wasm` 跑 `game_init` → 循环跑 `game_update`（每帧 `glFlush` → `glcmd_publish`）

**要点**：wasm 与渲染零改动；引擎只影响"谁解释 wasm 指令"。

## §2 命令流同步（N 缓冲零拷贝 paced，N 可配）

**现状开销**：`glcmd_publish()` 每帧 `memcpy(s_buf→s_pub)` ~4.5KB + 屏障。

**新设计**（single-producer / single-consumer，零拷贝 ping-pong，**缓冲数可配置** `GLCMD_NUM_BUFFERS`，默认 2，支持 2 或 3）：

```
N 个 8KB 缓冲 s_buf[N]，各配 volatile 长度槽 s_len[N]
（0 = 已消费可写，>0 = 已发布待消费）

编码端 core0（独占写 enc_idx，轮流）：
  glcmd_begin_frame():
    while (s_len[enc_idx] != 0) {}   // 稳态 30fps 锁步：恒为 0，零自旋；渲染卡顿时等待=正确（不产无用帧）
    s_pos = 0
  glcmd_publish():
    s_len[enc_idx] = s_pos;
    __sync_synchronize();            // 1 条 memw
    enc_idx = (enc_idx + 1) % N;

解码端 core1（独占写 dec_idx，轮流）：
  帧循环:
    if (s_len[dec_idx]) { glcmd_replay(s_buf[dec_idx], s_len[dec_idx]); s_len[dec_idx] = 0; }
    dec_idx = (dec_idx + 1) % N;
```

**性质**：
- 零拷贝（省 memcpy）、无锁、无临界区
- 同步开销 = 1 条 `memw` + 2 次 volatile 读（稳态）
- **paced**：编码端无空闲缓冲就不产帧 → 每一帧都被渲染，无无用帧
- 单核写各自状态 → 无数据竞争、无死锁（解码端独立核永续推进）
- 缓冲数编译期可配：`GLCMD_NUM_BUFFERS=2`（默认，paced）或 `3`（多一层抖动吸收）；内存占用 N×8KB

**API 变化**：`glcmd_stream.h` 暴露 `glcmd_begin_frame/publish`（编码）与 `glcmd_frame_poll/release`（解码端按序取帧），`glcmd_frame_len/frame_buf/frame_clear` 重构。

## §3 显示流水线（3 帧缓冲 + 过去帧）

**现状**：单 fb（40KB，SRAM，DMA），`wait_dma` 串行：渲染等 DMA → 无后处理余量。

**新设计**：N 帧缓冲流水线（**`DISPLAY_NUM_BUFFERS` 可配**，默认 3），**保留一个过去帧**，paced 无丢帧：

```
fb[0] ← DMA 传输中（上一帧）
fb[1] ← 后处理中（刚渲染帧）
fb[2] ← 光栅器渲染最新帧
+ 保留一个过去帧（供时序后处理：运动模糊 / 帧间混合）
每帧推进：渲染 → 后处理 → DMA → 显示，循环
```

**缓冲数可配置**：`DISPLAY_NUM_BUFFERS`（编译期）
- 2：渲染/后处理 ↔ DMA 重叠（后处理原地做），80KB
- 3（默认）：渲染/后处理/DMA 全并行 + 保留过去帧，120KB
- 内存占用 N×40KB SRAM

**要求**
- TinyGL `zb->pbuf` 需支持按当前渲染缓冲切换（历史单-fb 固定 pbuf 是双-fb 失败的根因）
- 显示 DMA 路径支持从任一 fb 缓冲读取
- **后处理钩子（可插拔，本次 no-op）**：`gl_post_process(src_fb, dst_fb, ...)`，后续迭代实现抖动/调色/模糊等效果
- paced：每帧都被显示，零丢弃

## §4 内存管理（`mem_mgr`，wasm 无感知）

**原则**：SRAM/PSRAM 取舍由 host 决定，wasm 游戏只看到标准线性内存 → 可移植。

**SRAM 分配**（图形引擎优先；fb/glcmd 缓冲数可配，按下表随 `DISPLAY_NUM_BUFFERS` / `GLCMD_NUM_BUFFERS` 变化）：

| 组件 | 默认大小 | 可配 |
|------|------|------|
| 图形引擎：fb N×40 + zbuf 40 + glcmd M×8 | 3×40 + 40 + 2×8 = **176KB** | fb N=2/3，glcmd M=2/3 |
| FreeRTOS 任务栈（游戏/渲染/主/空闲） | ~25KB | — |
| 余量（安全 + 未来纹理缓存） | ~40KB | 随 N/M 增减 |

**PSRAM 分配**：WAMR 全部（exec_env 操作栈 16–32KB、线性内存 128KB、堆 + 运行时结构）、其它 bulk。
- 理由：WAMR 每帧一次简单逻辑，非性能热点

**Flash**：纹理（10×48KB，已 30fps，保持 flash 采样）、代码、wasm 字节码。

**`mem_mgr` 机制**（host 层组件）：
- **已删除（实现时决策，YAGNI）**：内存路由由调用点内联完成——WAMR→PSRAM（`wasm_game.c` 内联分配器 + `wasm_runtime_full_init`），图形→SRAM（`display_st7735.c` fb + `gl_malloc` zbuf）。
```
void *mem_hot_alloc(size);    // → SRAM（图形引擎）
void *mem_bulk_alloc(size);   // → PSRAM（WAMR、大缓冲）
预算记账 + 超预算拒绝/降级
```
- WAMR 运行时通过 `wasm_runtime_set_allocator` 统一走 `mem_bulk_alloc`（PSRAM）
- 图形引擎保留现有 SRAM 分配（fb/zbuf/glcmd）
- 纹理缓存钩子 `mem_cached_alloc`（本次不实现，留接口）
- **LLM 预留：以后再说**，不占预算

**可移植性**：`mem_mgr` 只在 host；wasm 游戏用标准 wasm 内存语义。

## §5 清理

- 删除 `components/wasm3/`、`main/wasm3_game.{c,h}`
- **还原 128KB PSRAM 栈 hack**：`main/tinygl_test.c` 游戏任务回普通 SRAM 栈（~16KB），删 `WASM3_GAME_STACK_BYTES`/`s_wasm3_tcb`/`s_wasm3_stack`
- `sdkconfig.defaults`：删 `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`
- 删旧死代码 `main/wasm_game.c`（WAMR 示例）
- 更新 `.superpowers/sdd/progress.md` 记录进展

## 验证

1. 构建通过（`idf.py build`），wasm 重新生成（签名匹配 WAMR 原生）
2. 烧录后：`wasm_game` 任务启动日志、`game_init/game_update` 正常、30fps 场景正常、无栈溢出
3. `mem_mgr` 打印各池实际占用，确认 WAMR 在 PSRAM、图形在 SRAM
4. glcmd 双缓冲零拷贝：帧计数/丢帧行为正确（paced，无堆积）
5. 3-fb 显示流水线：显示正常无撕裂（DMA 从正确缓冲读）

## 明确不做 / 后续

- **后处理效果本身**（抖动/调色/模糊）：只搭 3-buffer 流水线框架 + no-op 钩子，效果后续迭代
- **脏矩形局部 DMA**：驱动已支持矩形接口（`hw_display_flush(x1,y1,x2,y2)`），但当前全屏天空盒每帧全变、无收益；等有静态区域场景再上脏矩形跟踪
- **LLM 集成**：以后再说，`mem_mgr` 留扩展位
- **金属立方体反光无效果**：独立渲染问题（同 wasm + 同 TinyGL），引擎切换不改变它，切换后继续排查
- **纹理缓存**：本次纹理保持 flash，`mem_cached_alloc` 留接口

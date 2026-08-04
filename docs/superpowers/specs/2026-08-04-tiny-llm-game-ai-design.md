# PC 原型：tiny LLM 游戏 AI 决策框架（C / llama2.c 风格）

**日期**: 2026-08-04
**分支**: 新分支 `llm-game-ai-pc`（实验/评估用）
**状态**: 已批准（进行实现规划前）

## 目标

在 PC 上验证"**LLM 决策层 + 规则引擎**"的游戏智能架构，为最终移植到 ESP32（小喵掌机固件，C 代码）做准备。约束：

1. **轻量级**：不用 llama.cpp 等重型运行时，用 tiny 级别的单文件 C LLM 推理框架。
2. **C 开发**：全部 C，代码直接可移植到 ESP32 固件。
3. **确保 ESP32 有跑起来的可能性**：框架单文件 C、模型可换更小量化版、LLM 接口抽象。

## 架构（每回合 3 个 LLM 上下文）

```
┌─ LLM #1 玩家选项上下文 ───────────────┐
│  llm_player_options(state) → 选项列表   │
└──────────────────────────────────────┘
┌─ LLM #2 NPC 行为上下文 ───────────────┐
│  llm_npc_behavior(state) → NPC 动作    │
└──────────────────────────────────────┘
        ↓ 玩家选一个 + NPC 动作
┌─ 规则引擎（确定性）───────────────────┐
│  rule_engine_apply(state, 选项, NPC)  │
│  → 效果事件（HP/道具/事件/胜负…）      │
└──────────────────────────────────────┘
┌─ LLM #3 解释上下文 ──────────────────┐
│  llm_explain(事件) → 自然语言叙述      │
└──────────────────────────────────────┘
```

- **LLM 提供智能**（动态选项、NPC 意图、结果叙述），**规则引擎保证确定性**（意图 → 实际游戏效果）。
- 三次 LLM 调用各用独立系统提示（上下文），互不干扰。

## 组件与文件

新分支 `llm-game-ai-pc`，目录 `pc-llm-ai/`：

| 文件 | 职责 |
|------|------|
| `CMakeLists.txt` | gcc 单文件构建，可开 `-O2` |
| `llm.c` / `llm.h` | **tiny 单文件 C 推理内核**：tokenizer + transformer 前向 + 采样（llama2.c 的 `run.c` 风格，MIT，适配为 `llm.c`），读 `.bin` 模型 |
| `llm_bridge.c` / `.h` | 三个入口：`llm_player_options(state)` / `llm_npc_behavior(state)` / `llm_explain(events)`——各自构造 prompt、调推理、解析输出；唯一 LLM 接口，未来 ESP32 换运行时只改这里 |
| `rule_engine.c` / `.h` | 手写 C 前向链规则引擎（~150 行，零依赖）：`rule_engine_apply(state, player_choice, npc_action) → events[]` |
| `rules.c` | 游戏规则集（条件→效果数组） |
| `game_sim.c` / `.h` | 文本冒险：`game_state_t`（HP/位置/道具/回合）+ 回合循环 |
| `main.c` | 入口：跑 N 回合，打印叙述与状态 |
| `models/stories15M.bin` | tiny 模型（llama2.c 预训练，从 repo release 下载） |

**数据流（每回合）**
```
打印状态 → llm_player_options → 玩家终端输入选择
        → llm_npc_behavior → rule_engine_apply → events
        → llm_explain(events) → 打印叙述 → 更新 state → 下一回合
```

## 关键决策

- **LLM 输出限定为枚举/短 token**：玩家选项是编号列表，NPC 动作是固定动作集（`attack/flee/talk/...`），规则引擎只认这些——确定性。
- **解释是自由文本**：`llm_explain` 输出自然语言叙述，供玩家阅读，不参与逻辑。
- **模型**：PC 验证用 `stories15M.bin`（15M 参数 fp32 ≈ 60MB）。框架是 ESP32 可移植的；ESP32 侧换更小量化模型（接口不变）。记录单次决策延迟 + 模型大小。
- **规则引擎零依赖纯 C**：`{条件, 效果}` 数组前向链，可原样编译进 ESP32。
- **LLM 推理封装**：llm_bridge 内部持有推理上下文（一次加载模型，多次生成），回合间不重建。

## 验证

1. PC 编译运行（`cmake -B build && cmake --build build`），无警告。
2. 每回合 LLM 真实推理：玩家选项合理、NPC 行为一致、叙述连贯。
3. 规则引擎把决策转成可观察效果（HP 变化/事件/胜负），N 回合稳定、无崩溃。
4. 测出单次 LLM 决策延迟 + 模型大小 → 写入评估结论（ESP32 可行性）。

## 明确不做

- 不做真实图形（终端文本即可，图形是 ESP32 端 TinyGL 的事）。
- 不实现 ESP32 移植本身（本原型只验证框架与架构）。
- 不做复杂规则语言（纯 C 结构数组足够）。

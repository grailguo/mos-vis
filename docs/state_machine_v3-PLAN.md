### `docs/state_machine_v3.md` 生成计划 (已实现)

> **注意**：本计划文档已执行完成，完整实现参见 [state_machine_v3.md](./state_machine_v3.md)。本文档保留供历史参考。

### Summary
产出一份新的状态机设计文档 `docs/state_machine_v3.md`，用于替代现有 v1/v2 的分散描述，聚焦“主状态机 + 子状态机（wake/asr/reply）”双层实现，并明确三项核心需求：
1. 一次唤醒多轮交互  
2. `WaitingCommandSpeech` 连续 15 秒无语音回到 `WaitingWakeup`  
3. 在 `WaitingCommandSpeech` 再次命中唤醒词时主动播 ACK “我在，请说。”

### Key Changes
1. 文档结构重构为单一权威说明
- 章节顺序：
  1. 目标与范围  
  2. 术语映射（文档状态名 ↔ 代码状态名）  
  3. 双层架构总览（主状态机/子状态机职责边界）  
  4. 主状态全集与事件全集  
  5. 转移优先级与执行顺序（含 `kAny` 全局规则）  
  6. 核心场景时序（多轮交互、15秒超时、二次唤醒ACK）  
  7. Guard 与 Action 语义规范  
  8. 日志与可观测性约束  
  9. 与现有 pipeline 的落地映射  
  10. 验收用例与回归清单

2. 主状态机内容按 v2 收敛但做实现约束补全
- 明确主状态全集采用文档扩展态（`Booting...ShuttingDown`）。
- 明确“Stage 只能投事件，不直接改主状态”。
- 明确 `CommandWaitTimeout` 只在 `WaitingCommandSpeech` 生效，默认 `15000ms`。
- 明确“二次唤醒 ACK”只在 `WaitingCommandSpeech` 触发，其他状态忽略。

3. 子状态机与主状态机的协同协议
- `wake/asr/reply` 子状态机仅产出局部决策（decision/action），不直接持有主状态。
- 主状态机统一消费子状态机产物，执行主状态转移和动作副作用。
- 定义事件总线字段：`event_type`, `source_stage`, `session_id`, `turn_id`, `payload`, `ts_ms`。

4. 与代码映射的约束表
- 添加“文档状态 -> 当前 pipeline 阶段触发点 -> 预期 action”映射表。
- 明确 `ControlNotification` 在 v3 中归属于 `WaitingControlAsync` 事件源，不绕过主状态机。
- 添加“未实现能力标记”（RAG/LLM 可先占位事件，业务处理后接入）。

### Test Plan
1. 文档一致性检查
- 状态名、事件名、guard/action 命名在全篇唯一且无同义重复。
- 转移表中每个状态至少有一个退出路径（除终态）。
2. 核心需求检查
- 多轮交互路径闭环完整（`ResultSpeaking -> WaitingCommandSpeech`）。
- 15 秒超时路径唯一且明确回 `WaitingWakeup`。
- 二次唤醒词仅在 `WaitingCommandSpeech` 触发 ACK。
3. 实现可落地性检查
- 每个 Action 有唯一责任归属（主状态机/某 Stage）。
- 每个关键转移有对应日志字段规范（`from/event/guard/to/action`）。

### Assumptions
- 本文档是“v3 设计基线”，后续代码按文档对齐；文档优先级高于旧版说明。
- ACK 文案固定为“我在，请说。”，可在配置层再做关键词定制。
- 本次先产出设计文档，不同时修改代码实现。

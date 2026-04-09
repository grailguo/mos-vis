mos_vis 语音交互系统状态机定义文档 v3
====================

## 文档状态
- **版本**: v3
- **状态**: 设计文档（当前实现基线 + 目标架构）
- **替代**: 本文档旨在替代 `state_machine.md` (v1) 和 `state_machine_transition_map_v2.md`
- **最后更新**: 2026-04-09

## 摘要
本文档定义 mos_vis 语音交互系统的第三代状态机设计，采用“主状态机 + 子状态机（wake/asr/reply）”双层架构，明确三项核心需求：

1. **一次唤醒多轮交互** - 单次唤醒后支持连续命令输入
2. **`WaitingCommandSpeech` 连续 15 秒无语音回到 `WaitingWakeup`** - 命令等待超时机制
3. **在 `WaitingCommandSpeech` 再次命中唤醒词时主动播 ACK "我在，请说。"** - 二次唤醒确认

本文档包含当前实现基线（8 状态分布式管理）与目标架构（16 状态集中式管理）的映射关系，为后续代码迁移提供清晰路径。

---

## 目录
1. [目标与范围](#1-目标与范围)
2. [当前实现基线](#2-当前实现基线)
3. [术语映射](#3-术语映射)
4. [双层架构总览](#4-双层架构总览)
5. [主状态全集与事件全集](#5-主状态全集与事件全集)
6. [转移优先级与执行顺序](#6-转移优先级与执行顺序)
7. [核心场景时序](#7-核心场景时序)
8. [Implementation Gaps](#8-implementation-gaps)
9. [Guard 与 Action 语义规范](#9-guard-与-action-语义规范)
10. [日志与可观测性约束](#10-日志与可观测性约束)
11. [与现有 pipeline 的落地映射](#11-与现有-pipeline-的落地映射)
12. [迁移考虑事项](#12-迁移考虑事项)
13. [验收用例与回归清单](#13-验收用例与回归清单)

---

## 1. 目标与范围

### 1.1 目标
定义 mos_vis 语音交互系统的统一状态机设计，协调以下模块：
- AudioCapture / AudioRingBuffer
- VadEngine (vad1_, vad2_)
- SherpaKwsEngine
- SherpaAsrEngine  
- SimpleNluEngine
- SimpleControlEngine
- StubRagEngine / StubLlmEngine
- TtsEngine

### 1.2 范围
- 业务状态管理（非线程/引擎生命周期）
- 事件驱动状态转移
- 超时与会话策略
- 错误恢复机制

### 1.3 设计原则
- **状态机只管理业务阶段**：状态表示系统当前业务阶段，不直接等同于底层引擎运行状态
- **Pipeline 负责连续数据流**：音频采集、缓存、处理由 pipeline 管理，状态机决定哪些模块有权消费
- **明确 TTS 播放态**：播报期间必须有显式状态，避免 KWS/ASR 误触发
- **已唤醒会话有超时窗口**：唤醒后进入命令等待窗口，超时无输入则退回待唤醒
- **控制命令支持同步确认与异步完成通知**：两阶段反馈机制

---

## 2. 当前实现基线

> **重要**：当前 mos_vis 代码库实际使用的是 8 状态分布式管理架构，与文档 v1/v2 的 16 状态设计存在差异。本节描述当前实现现实。

### 2.1 主会话状态（8 状态）
当前 `SessionState` 枚举（`include/mos/vis/runtime/session_state.h`）：
```cpp
enum class SessionState {
  kIdle,           // 待唤醒（对应 v3 的 kWaitingWakeup）
  kWakeCandidate,  // 唤醒候选检测中（对应 v3 的 kWakeDetecting）
  kPreListening,   // 唤醒确认后等待命令（对应 v3 的 kAckSpeaking + kWaitingCommandSpeech）
  kListening,      // 命令语音识别中（对应 v3 的 kRecognizingCommand）
  kFinalizing,     // ASR 结束处理中
  kRecognizing,    // 语义理解中（对应 v3 的 kUnderstandingIntent）
  kExecuting,      // 命令执行中（对应 v3 的 kExecutingControlSync/kWaitingControlAsync）
  kSpeaking        // 结果播报中（对应 v3 的 kResultSpeaking）
};
```

### 2.2 子状态机（Hotspot Sub-SMs）
当前实现使用三个"热点"子状态机处理复杂行为：
1. **Wake 子状态机** (`WakeState`): `kWaitingWakeup`, `kWakeDetecting`
2. **ASR 子状态机** (`AsrState`): `kListening`, `kFinalizing`, `kRecognizing`
3. **Reply 子状态机** (`ReplyState`): `kResultSpeaking`

### 2.3 分布式状态管理
当前状态转移逻辑分散在各 pipeline stage 中：
- **KWS Stage**: 处理唤醒检测和状态转移到 `kPreListening`
- **ASR Stage**: 处理命令识别和 15 秒超时
- **TTS Stage**: 处理播报完成和多轮会话策略

### 2.4 事件总线
`LocalEventBus` (`include/mos/vis/runtime/session_context.h`) 提供三个独立队列：
- `wake_events`: Wake 子状态机事件
- `asr_events`: ASR 子状态机事件  
- `reply_events`: Reply 子状态机事件

---

## 3. 术语映射

| 文档状态名 | 代码状态名 (当前) | 代码状态名 (目标 v3) | 说明 |
|------------|-------------------|----------------------|------|
| Booting | (无) | kBooting | 系统启动初始化 |
| WaitingWakeup | kIdle | kWaitingWakeup | 待唤醒状态 |
| WakeDetecting | kWakeCandidate | kWakeDetecting | 唤醒词检测中 |
| AckSpeaking | (kPreListening 中的 TTS 任务) | kAckSpeaking | 播报唤醒确认 |
| WaitingCommandSpeech | kPreListening | kWaitingCommandSpeech | 等待命令语音 |
| RecognizingCommand | kListening + kFinalizing + kRecognizing | kRecognizingCommand | 命令语音识别 |
| UnderstandingIntent | kRecognizing | kUnderstandingIntent | 语义理解 |
| ExecutingControlSync | kExecuting | kExecutingControlSync | 控制命令同步执行 |
| WaitingControlAsync | (kExecuting 中异步等待) | kWaitingControlAsync | 等待异步完成通知 |
| QueryingRag | (未实现) | kQueryingRag | RAG 查询中 |
| ChattingLlm | (未实现) | kChattingLlm | LLM 对话中 |
| ResultSpeaking | kSpeaking | kResultSpeaking | 结果播报中 |
| ErrorRecovery | (未实现) | kErrorRecovery | 错误恢复中 |
| ShuttingDown | (未实现) | kShuttingDown | 系统关闭中 |
| kAny | (无) | kAny | 全局转移匹配符 |

---

## 4. 双层架构总览

### 4.1 总体架构
```
┌─────────────────────────────────────────────────┐
│              主状态机 (16 状态)                  │
│  Booting → WaitingWakeup → WakeDetecting → ...  │
└───────────────┬─────────────────┬───────────────┘
                │                 │
        ┌───────▼─────┐   ┌───────▼─────┐
        │ Wake 子状态机 │   │ ASR 子状态机 │
        │ - Waiting   │   │ - Listening │
        │ - Detecting │   │ - Finalizing│
        └─────────────┘   └─────────────┘
                │                 │
        ┌───────▼─────────────────▼───────┐
        │         Reply 子状态机           │
        │       - ResultSpeaking          │
        └─────────────────────────────────┘
```

### 4.2 职责划分
- **主状态机**: 管理业务阶段流转，处理高级别事件，维护会话上下文
- **子状态机**: 处理"热点"复杂行为，产出局部决策（decision/action），不直接持有主状态
- **Pipeline Stages**: 只投递事件，不直接修改主状态；根据当前状态决定是否处理数据

### 4.3 当前 vs 目标架构映射
| 当前实现 | 目标 v3 架构 | 差异说明 |
|----------|--------------|----------|
| 8 主状态 + 3 子状态机 | 16 主状态 + 3 子状态机 | 状态粒度更细，异步操作显式建模 |
| 状态转移分散在 stages | 集中式状态机控制器 | 提高可维护性和可测试性 |
| 独立事件队列 | 统一事件总线 | 简化事件分发逻辑 |
| 超时逻辑在各 stage | 集中超时管理器 | 统一超时处理机制 |

---

## 5. 主状态全集与事件全集

### 5.1 主状态全集 (v3 目标)
```cpp
enum class VisState {
  kBooting,
  kWaitingWakeup,
  kWakeDetecting,
  kAckSpeaking,
  kWaitingCommandSpeech,
  kRecognizingCommand,
  kUnderstandingIntent,
  kExecutingControlSync,
  kWaitingControlAsync,
  kQueryingRag,
  kChattingLlm,
  kResultSpeaking,
  kErrorRecovery,
  kShuttingDown,   // 终态
  kAny,            // 仅用于转移表匹配，不作为运行态
};
```

### 5.2 事件全集 (v3 目标)
```cpp
enum class VisEventType {
  // 生命周期事件
  kBootCompleted, kBootFailed, kShutdownRequested, kFatalError,
  kRecoverySucceeded, kRecoveryFailed,
  
  // 唤醒相关事件
  kWakeSpeechDetected, kKwsMatched, kKwsRejected, kKwsTimeout,
  
  // 命令等待相关事件  
  kCommandSpeechDetected, kCommandWaitTimeout,
  
  // ASR 相关事件
  kAsrPartialResult, kAsrFinalResult, kAsrEndpointWithEmptyText,
  kAsrTimeout, kAsrError, kUserBargeIn,
  
  // NLU 相关事件
  kIntentControl, kIntentRag, kIntentLlm, kIntentUnknown,
  kNluError, kNluTimeout,
  
  // 控制相关事件
  kControlSyncAckSuccess, kControlSyncAckFail, kControlSyncAckTimeout,
  kControlTransportError, kControlAsyncSuccess, kControlAsyncFail,
  kControlAsyncTimeout, kControlTransportDisconnected,
  kControlAsyncStaleOrMismatched,
  
  // RAG/LLM 相关事件
  kRagSuccess, kRagEmpty, kRagTimeout, kRagError,
  kLlmSuccess, kLlmTimeout, kLlmError,
  
  // TTS 相关事件
  kTtsPlaybackCompleted, kTtsPlaybackFailed,
};
```

### 5.3 事件结构
```cpp
struct VisEvent {
  VisEventType type;
  std::string text;           // ASR 文本、回复文本等
  std::string request_id;     // 用于异步操作匹配
  std::string source_stage;   // 事件源阶段标识
  int64_t session_id;         // 会话 ID
  int32_t turn_id;            // 轮次 ID
  std::string payload;        // 扩展载荷（JSON 等）
  int64_t ts_ms;              // 时间戳（毫秒）
};
```

---

## 6. 转移优先级与执行顺序

### 6.1 转移匹配优先级
1. **全局规则优先**: 先查找 `{from = kAny, event = e.type}` 的转移
2. **状态特定规则**: 再查找 `{from = current_state, event = e.type}` 的转移  
3. **Guard 顺序匹配**: 对同状态同事件的多个转移，按表中顺序检查 guard，取首个成立者
4. **无匹配处理**: 若无匹配转移，记录 `UnhandledEvent` 日志，保持原状态

### 6.2 kAny 全局规则
以下事件在任何状态下都应被处理：
- `kShutdownRequested` → `kShuttingDown` (停机请求)
- `kFatalError` → `kErrorRecovery` (致命错误)
- `kUserBargeIn` (在 `kAckSpeaking` 或 `kResultSpeaking` 状态下触发 ASR)

### 6.3 执行顺序约束
- `HandleEvent()` 禁止重入，防止递归调用
- Action 内产生的新事件一律投递到事件队列，不允许同步再次调用 `HandleEvent()`
- 状态转移顺序: `OnExit(old_state)` → `RunAction(action)` → `current_state = new_state` → `OnEnter(new_state)`

---

## 7. 核心场景时序

### 7.1 一次唤醒多轮交互
```
用户: [唤醒词] "小莫"
系统: "我在，请说。" (kAckSpeaking → kWaitingCommandSpeech)
用户: "打开灯光"
系统: "已打开灯光" (kResultSpeaking → kWaitingCommandSpeech)  # keep_session_open = true
用户: "调亮一点"
系统: "已调亮" (kResultSpeaking → kWaitingCommandSpeech)
用户: (15 秒无语音)
系统: (超时) → kWaitingWakeup
```

### 7.2 WaitingCommandSpeech 15 秒超时
- 进入 `kWaitingCommandSpeech` 时启动 `command_wait_timeout = 15000ms`
- 用户开始说话 (`kCommandSpeechDetected`) 时停止计时器
- 超时触发 `kCommandWaitTimeout` → 转移到 `kWaitingWakeup`
- **当前实现**: ASR Stage 的 `HandleNoTextTimeout()` 实现 15 秒超时，但回到 `kPreListening` 而非 `kIdle`

### 7.3 二次唤醒 ACK
- 在 `kWaitingCommandSpeech` 状态中再次命中唤醒词
- 触发 `kKwsMatched` 事件
- 执行 Action: `kPlayAck` (播放 "我在，请说。")
- 状态转移: `kWaitingCommandSpeech` → `kAckSpeaking` → `kWaitingCommandSpeech`
- **当前实现**: KWS Stage 仅处理 `kIdle` 状态，未实现此功能

---

## 8. Implementation Gaps

### 8.1 核心需求实现状态
| 需求 | 当前实现状态 | 缺失部分 |
|------|--------------|----------|
| 一次唤醒多轮交互 | ✅ 部分实现 | `keep_session_open` 策略需标准化 |
| WaitingCommandSpeech 15 秒超时 | ✅ 部分实现 | 超时应回到 `kIdle` (对应 `kWaitingWakeup`) |
| 二次唤醒 ACK | ❌ 未实现 | KWS Stage 需支持在 `kPreListening` 状态处理唤醒词 |

### 8.2 架构差异缺口
1. **状态粒度**: 当前 8 状态 vs 目标 16 状态
2. **异步操作建模**: 当前 `kExecuting` 单状态 vs 目标 `kExecutingControlSync` + `kWaitingControlAsync`
3. **错误恢复**: 当前无专门错误恢复状态
4. **RAG/LLM 支持**: 当前未实现对应状态

### 8.3 事件系统缺口
1. **事件类型覆盖**: 当前事件类型有限，缺少完整 v3 事件集
2. **事件总线统一**: 当前独立队列 vs 目标统一事件总线
3. **请求 ID 匹配**: 当前部分实现 request_id 跟踪

---

## 9. Guard 与 Action 语义规范

### 9.1 Guard 类型
```cpp
enum class GuardType {
  kAlways,
  
  // Wake guards
  kNotInWakeCooldown, kInWakeCooldown,
  
  // ASR guards  
  kAsrFinalTextNonEmpty, kAsrFinalTextEmpty,
  
  // Fallback / session policy
  kFallbackLlmEnabled, kFallbackLlmDisabled,
  kKeepSessionOpen, kNotKeepSessionOpen,
  
  // Async control guards
  kControlAsyncEventMatchesCurrent, kControlAsyncEventNotMatchesCurrent,
};
```

### 9.2 Guard 语义实现
```cpp
// 示例实现
bool EvaluateGuard(GuardType guard, const VisEvent& ev, const VisContext& ctx, const Config& config) {
  switch (guard) {
    case GuardType::kNotInWakeCooldown:
      return !ctx.InWakeCooldown();
    case GuardType::kAsrFinalTextNonEmpty:
      return !Trim(ev.text).empty();
    case GuardType::kKeepSessionOpen:
      return ctx.keep_session_open;
    case GuardType::kControlAsyncEventMatchesCurrent:
      return ev.request_id == ctx.current_control_request_id;
    // ... 其他 guard
  }
}
```

### 9.3 Action 类型 (关键子集)
```cpp
enum class ActionType {
  kNoop,
  
  // Boot / shutdown / recovery
  kInitDone, kRecordBootError, kRunErrorRecovery, kEnterShuttingDown,
  
  // Wake / KWS
  kOpenKwsWindow, kCleanupKwsWindow, kPlayAck,
  
  // Command window / session
  kEnterWaitingCommandWindow, kEndSessionToWakeup, kEndSessionOrRearmCommandWait,
  
  // ASR / NLU
  kStartAsrForCommand, kUpdateAsrPartial, kSubmitAsrFinalToNlu,
  kCleanupAsrAndRearmCommandWait, kPrepareReplyAsrRetry, kPrepareReplyUnknown,
  
  // Control
  kSendControlRequest, kStartWaitingAsyncNotify, kPrepareReplyControlFail,
  kPrepareReplyControlTimeout, kPrepareReplyControlTransportError,
  kPrepareReplyControlDone, kDiscardStaleControlAsyncEvent,
  
  // RAG / LLM
  kStartRagQuery, kStartLlmChat, kPrepareReplyQueryResult,
  kPrepareReplyQueryEmpty, kPrepareReplyQueryFail, kPrepareReplyLlmResult,
  kPrepareReplyLlmFail,
  
  // TTS
  kPlayReply, kStopCurrentTtsAndStartAsr,
};
```

### 9.4 关键 Action 语义
- **`kPlayAck`**: 播放唤醒确认语音，当前使用 `ResolveWakeAckText()` 解析文本
- **`kEnterWaitingCommandWindow`**: 启动/重启 `command_wait_timeout = 15000ms`
- **`kStopCurrentTtsAndStartAsr`**: 用户抢话打断时停止当前 TTS 并启动 ASR
- **`kPrepareReplyControlDone`**: 根据控制结果设置 `ctx.keep_session_open` 策略

---

## 10. 日志与可观测性约束

### 10.1 必须日志字段
每个状态转移应记录：
```json
{
  "timestamp": "2026-04-09T10:30:00Z",
  "session_id": "sess_123",
  "turn_id": 1,
  "from_state": "kWaitingCommandSpeech",
  "event": "kCommandWaitTimeout",
  "guard": "kAlways",
  "to_state": "kWaitingWakeup",
  "action": "kEndSessionToWakeup",
  "event_source": "KwsStage",
  "request_id": "req_456"
}
```

### 10.2 性能指标
- 状态驻留时间分布
- 事件处理延迟 (入队到处理完成)
- 超时触发次数统计
- 未处理事件计数

### 10.3 调试支持
- 状态机当前状态可查询接口
- 事件队列深度监控
- 上下文字段快照能力

---

## 11. 与现有 pipeline 的落地映射

### 11.1 Pipeline Stages 与状态关系
| Stage | 当前状态依赖 | 事件产出 | 目标 v3 映射 |
|-------|--------------|----------|-------------|
| KWS Stage | `kIdle` 状态处理 | `kWakeSpeechDetected`, `kKwsMatched` | 需支持 `kWaitingCommandSpeech` 状态 |
| VAD1 Stage | 始终运行 | `kWakeSpeechDetected` | 无变化 |
| VAD2 Stage | `kPreListening` 状态 | `kCommandSpeechDetected` | 对应 `kWaitingCommandSpeech` |
| ASR Stage | `kListening`, `kFinalizing` | `kAsrPartialResult`, `kAsrFinalResult`, `kAsrTimeout` | 对应 `kRecognizingCommand` |
| Recognizing Stage | `kRecognizing` | `kIntentControl`, `kIntentRag`, `kIntentLlm` | 对应 `kUnderstandingIntent` |
| Executing Stage | `kExecuting` | 控制相关事件 | 对应 `kExecutingControlSync` + `kWaitingControlAsync` |
| TTS Stage | 始终运行 | `kTtsPlaybackCompleted`, `kTtsPlaybackFailed` | 对应 `kAckSpeaking` + `kResultSpeaking` |
| Control Notification Stage | 异步通知处理 | `kControlAsyncSuccess`, `kControlAsyncFail` | 对应 `kWaitingControlAsync` 状态 |

### 11.2 关键文件映射
| 文件路径 | 当前职责 | v3 对应职责 |
|----------|----------|-------------|
| `include/mos/vis/runtime/session_state.h` | 8 状态定义 | 需扩展为 16 状态 |
| `include/mos/vis/runtime/session_context.h` | 会话上下文 | 需添加 v3 上下文字段 |
| `include/mos/vis/runtime/subsm/hotspot_subsms.h` | 子状态机定义 | 可重用，需与主状态机集成 |
| `include/mos/vis/runtime/subsm/transition_map.h` | 通用转移表 | v3 状态机核心基础设施 |
| `include/mos/vis/runtime/stages/kws_stage.h` | KWS 处理 | 需支持二次唤醒 ACK |
| `include/mos/vis/runtime/stages/asr_stage.h` | ASR 处理 | 15 秒超时逻辑调整 |
| `include/mos/vis/runtime/stages/tts_stage.h` | TTS 处理 | 多轮会话策略标准化 |
| `src/runtime/session_controller.cc` | 会话控制 | 需集成状态机控制器 |

### 11.3 ControlNotification 归属
- **当前**: `ControlNotification` 可能绕过状态机直接处理
- **v3 规范**: 所有 `ControlNotification` 必须包装为 `VisEvent` 并投递到状态机
- **归属规则**: 在 `kWaitingControlAsync` 状态中，只有 `request_id` 匹配的事件才触发转移

---

## 12. 迁移考虑事项

### 12.1 迁移策略
1. **增量迁移**: 先补充缺失功能（二次唤醒 ACK），再逐步重构架构
2. **兼容模式**: 新状态机控制器与现有 pipeline 并行运行，逐步接管状态转移
3. **配置开关**: 通过配置项选择使用当前实现或 v3 状态机

### 12.2 风险点
- **状态转移时序**: 集中式状态机可能改变事件处理时序
- **性能影响**: 额外事件封装和分发可能增加延迟
- **测试覆盖**: 需要充分测试确保现有功能不受影响

### 12.3 推荐步骤
1. **阶段 1**: 实现二次唤醒 ACK，修复 15 秒超时回 `kIdle`
2. **阶段 2**: 实现 `StateMachineController` 原型，处理部分状态转移
3. **阶段 3**: 逐步迁移各 pipeline stage 到新状态机
4. **阶段 4**: 完整实现 16 状态，移除旧状态管理逻辑

---

## 13. 验收用例与回归清单

### 13.1 核心需求验收用例
1. **UC-01 多轮交互**
   - 唤醒 → 命令 → 回复 → 保持会话 → 第二条命令 → 回复 → 超时返回待唤醒
   - 验证 `keep_session_open` 策略正确性

2. **UC-02 15 秒超时**
   - 唤醒 → 等待 16 秒 → 自动返回待唤醒
   - 验证超时计时器正确启停

3. **UC-03 二次唤醒 ACK**
   - 唤醒 → 等待命令 → 再次说唤醒词 → 听到 "我在，请说。" → 可继续说命令
   - 验证 KWS 在 `kWaitingCommandSpeech` 状态有效

### 13.2 回归测试清单
- [ ] 基本唤醒流程 (唤醒词 → ACK → 待命)
- [ ] 命令识别流程 (说话 → ASR → NLU → 执行)
- [ ] 控制命令同步+异步流程
- [ ] TTS 播报与抢话打断
- [ ] 错误恢复与重试
- [ ] 会话超时与清理
- [ ] 多轮对话上下文保持

### 13.3 性能回归指标
- 唤醒延迟 ≤ 500ms
- 端到端命令响应时间 ≤ 3s
- 状态转移延迟 ≤ 50ms
- 内存占用增长 ≤ 10%

---

## 附录 A: 当前实现代码引用

### A.1 关键代码位置
1. **SessionState 枚举**: `include/mos/vis/runtime/session_state.h`
2. **子状态机定义**: `include/mos/vis/runtime/subsm/hotspot_subsms.h`
3. **KWS Stage 状态检查**: `src/runtime/stages/kws_stage.cc:240` (`if (context.state == SessionState::kIdle)`)
4. **ASR Stage 15 秒超时**: `src/runtime/stages/asr_stage.cc` `HandleNoTextTimeout()`
5. **TTS Stage 多轮策略**: `src/runtime/stages/tts_stage.cc` `keep_session_open` 处理

### A.2 配置项引用
- `command_wait_timeout_seconds`: 命令等待超时（当前 15 秒）
- `wake_ack_text`: 唤醒确认文本（可配置）
- `keep_session_open_default`: 默认会话保持策略

---

## 文档修订历史
| 版本 | 日期 | 修改说明 | 修改人 |
|------|------|----------|--------|
| v3.0 | 2026-04-09 | 初始版本，包含当前实现基线与目标架构 | Claude Code |
| v3.1 | (待定) | 根据实际代码迁移更新 | |

---

**文档结束**
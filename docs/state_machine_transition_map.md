mos_vis 状态机到 C++ 枚举/Transition Map 映射（v1）

1. 目标

本文档给出可直接落地到 C++ 的状态机定义：
- 状态枚举
- 事件枚举
- Guard/Action 枚举
- Transition Map（表驱动）

来源语义与 [state_machine_v3.md](./state_machine_v3.md) 保持一致（本文档为 v1 历史版本，详细设计请参考 v3）。

2. 推荐数据结构

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

enum class VisEventType {
  kBootCompleted,
  kBootFailed,
  kShutdownRequested,
  kFatalError,
  kRecoverySucceeded,
  kRecoveryFailed,
  kWakeSpeechDetected,
  kKwsMatched,
  kKwsRejected,
  kKwsTimeout,
  kCommandSpeechDetected,
  kCommandWaitTimeout,
  kAsrPartialResult,
  kAsrFinalResult,
  kAsrEndpointWithEmptyText,
  kAsrTimeout,
  kAsrError,
  kIntentControl,
  kIntentRag,
  kIntentLlm,
  kIntentUnknown,
  kNluError,
  kControlSyncAckSuccess,
  kControlSyncAckFail,
  kControlSyncAckTimeout,
  kControlTransportError,
  kControlAsyncSuccess,
  kControlAsyncFail,
  kControlAsyncTimeout,
  kControlTransportDisconnected,
  kRagSuccess,
  kRagEmpty,
  kRagTimeout,
  kRagError,
  kLlmSuccess,
  kLlmTimeout,
  kLlmError,
  kTtsPlaybackCompleted,
  kTtsPlaybackFailed,
};

enum class GuardType {
  kAlways,
  kNotInWakeCooldown,  // !ctx.InWakeCooldown()
  kInWakeCooldown,     //  ctx.InWakeCooldown()
  kAsrFinalTextNonEmpty,
  kFallbackLlmEnabled,
  kFallbackLlmDisabled,
};

enum class ActionType {
  kNoop,
  kInitDone,
  kRecordBootError,
  kOpenKwsWindow,
  kPlayAck,
  kCleanupKwsWindow,
  kEnterWaitingCommandWindow,      // 启动/重启 15s command_wait_timeout
  kEndSessionToWakeup,
  kUpdateAsrPartial,
  kSubmitAsrFinalToNlu,
  kCleanupAsrAndRearmCommandWait,  // 清理 ASR + 重启 15s
  kSendControlRequest,
  kStartWaitingAsyncNotify,
  kPrepareReplyUnknown,
  kPrepareReplyControlFail,
  kPrepareReplyControlTimeout,
  kPrepareReplyControlTransportError,
  kPrepareReplyControlDone,
  kPrepareReplyQueryResult,
  kPrepareReplyQueryEmpty,
  kPrepareReplyQueryFail,
  kPrepareReplyLlmResult,
  kPrepareReplyLlmFail,
  kPlayReply,
  kRunErrorRecovery,
  kEnterShuttingDown,              // 停止消费事件并释放资源
};

struct VisEvent {
  VisEventType type;
  std::string text;
  std::string request_id;
};

struct Transition {
  VisState from;
  VisEventType event;
  GuardType guard;
  VisState to;
  ActionType action;
};
```

3. Transition Map（可直接初始化）

```cpp
constexpr Transition kTransitions[] = {
  // 全局高优先级规则（建议优先匹配）
  {VisState::kAny, VisEventType::kShutdownRequested, GuardType::kAlways, VisState::kShuttingDown, ActionType::kEnterShuttingDown},
  {VisState::kAny, VisEventType::kFatalError, GuardType::kAlways, VisState::kErrorRecovery, ActionType::kRunErrorRecovery},

  // Booting
  {VisState::kBooting, VisEventType::kBootCompleted, GuardType::kAlways, VisState::kWaitingWakeup, ActionType::kInitDone},
  {VisState::kBooting, VisEventType::kBootFailed, GuardType::kAlways, VisState::kErrorRecovery, ActionType::kRecordBootError},

  // WaitingWakeup
  {VisState::kWaitingWakeup, VisEventType::kWakeSpeechDetected, GuardType::kAlways, VisState::kWakeDetecting, ActionType::kOpenKwsWindow},

  // WakeDetecting
  {VisState::kWakeDetecting, VisEventType::kKwsMatched, GuardType::kNotInWakeCooldown, VisState::kAckSpeaking, ActionType::kPlayAck},
  {VisState::kWakeDetecting, VisEventType::kKwsMatched, GuardType::kInWakeCooldown, VisState::kWaitingWakeup, ActionType::kCleanupKwsWindow},
  {VisState::kWakeDetecting, VisEventType::kKwsRejected, GuardType::kAlways, VisState::kWaitingWakeup, ActionType::kCleanupKwsWindow},
  {VisState::kWakeDetecting, VisEventType::kKwsTimeout, GuardType::kAlways, VisState::kWaitingWakeup, ActionType::kCleanupKwsWindow},

  // AckSpeaking
  {VisState::kAckSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},
  {VisState::kAckSpeaking, VisEventType::kTtsPlaybackFailed, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},

  // WaitingCommandSpeech
  {VisState::kWaitingCommandSpeech, VisEventType::kCommandSpeechDetected, GuardType::kAlways, VisState::kRecognizingCommand, ActionType::kNoop},
  {VisState::kWaitingCommandSpeech, VisEventType::kCommandWaitTimeout, GuardType::kAlways, VisState::kWaitingWakeup, ActionType::kEndSessionToWakeup},

  // RecognizingCommand
  {VisState::kRecognizingCommand, VisEventType::kAsrPartialResult, GuardType::kAlways, VisState::kRecognizingCommand, ActionType::kUpdateAsrPartial},
  {VisState::kRecognizingCommand, VisEventType::kAsrFinalResult, GuardType::kAsrFinalTextNonEmpty, VisState::kUnderstandingIntent, ActionType::kSubmitAsrFinalToNlu},
  {VisState::kRecognizingCommand, VisEventType::kAsrEndpointWithEmptyText, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait},
  {VisState::kRecognizingCommand, VisEventType::kAsrTimeout, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait},
  {VisState::kRecognizingCommand, VisEventType::kAsrError, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait},

  // UnderstandingIntent
  {VisState::kUnderstandingIntent, VisEventType::kIntentControl, GuardType::kAlways, VisState::kExecutingControlSync, ActionType::kSendControlRequest},
  {VisState::kUnderstandingIntent, VisEventType::kIntentRag, GuardType::kAlways, VisState::kQueryingRag, ActionType::kNoop},
  {VisState::kUnderstandingIntent, VisEventType::kIntentLlm, GuardType::kAlways, VisState::kChattingLlm, ActionType::kNoop},
  {VisState::kUnderstandingIntent, VisEventType::kIntentUnknown, GuardType::kFallbackLlmEnabled, VisState::kChattingLlm, ActionType::kNoop},
  {VisState::kUnderstandingIntent, VisEventType::kIntentUnknown, GuardType::kFallbackLlmDisabled, VisState::kResultSpeaking, ActionType::kPrepareReplyUnknown},
  {VisState::kUnderstandingIntent, VisEventType::kNluError, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyUnknown},

  // ExecutingControlSync
  {VisState::kExecutingControlSync, VisEventType::kControlSyncAckSuccess, GuardType::kAlways, VisState::kWaitingControlAsync, ActionType::kStartWaitingAsyncNotify},
  {VisState::kExecutingControlSync, VisEventType::kControlSyncAckFail, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlFail},
  {VisState::kExecutingControlSync, VisEventType::kControlSyncAckTimeout, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlTimeout},
  {VisState::kExecutingControlSync, VisEventType::kControlTransportError, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlTransportError},

  // WaitingControlAsync
  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncSuccess, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlDone},
  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncFail, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlFail},
  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncTimeout, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlTimeout},
  {VisState::kWaitingControlAsync, VisEventType::kControlTransportDisconnected, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyControlTransportError},

  // QueryingRag
  {VisState::kQueryingRag, VisEventType::kRagSuccess, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyQueryResult},
  {VisState::kQueryingRag, VisEventType::kRagEmpty, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyQueryEmpty},
  {VisState::kQueryingRag, VisEventType::kRagTimeout, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyQueryFail},
  {VisState::kQueryingRag, VisEventType::kRagError, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyQueryFail},

  // ChattingLlm
  {VisState::kChattingLlm, VisEventType::kLlmSuccess, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyLlmResult},
  {VisState::kChattingLlm, VisEventType::kLlmTimeout, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyLlmFail},
  {VisState::kChattingLlm, VisEventType::kLlmError, GuardType::kAlways, VisState::kResultSpeaking, ActionType::kPrepareReplyLlmFail},

  // ResultSpeaking（多轮交互关键：回到 WaitingCommandSpeech 且重启 15s）
  {VisState::kResultSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},
  {VisState::kResultSpeaking, VisEventType::kTtsPlaybackFailed, GuardType::kAlways, VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},

  // ErrorRecovery
  {VisState::kErrorRecovery, VisEventType::kRecoverySucceeded, GuardType::kAlways, VisState::kWaitingWakeup, ActionType::kNoop},
  {VisState::kErrorRecovery, VisEventType::kRecoveryFailed, GuardType::kAlways, VisState::kShuttingDown, ActionType::kEnterShuttingDown},
};
```

4. 匹配优先级（必须固定）

为避免歧义，`HandleEvent()` 建议使用如下顺序：
1. 先查找 `{from = kAny, event = e.type}`（全局规则）
2. 再查找 `{from = current_state, event = e.type}` 且 guard 成立
3. 若无匹配，记录 `UnhandledEvent` 并保持原状态

5. Guard 与 Action 语义约定

- `kNotInWakeCooldown`/`kInWakeCooldown`：基于 `ctx.last_kws_timestamp` 与 `wake_cooldown_ms`
- `kAsrFinalTextNonEmpty`：`trim(event.text).empty() == false`
- `kFallbackLlmEnabled`：配置 `fallback_llm == true`
- `kEnterWaitingCommandWindow`：每次进入 `kWaitingCommandSpeech` 都执行，统一重启 `command_wait_timeout_ms`（默认 15000）
- `kEnterShuttingDown`：停止事件消费、停止运行中模块、释放资源；`kShuttingDown` 为终态

6. 最小执行框架（示意）

```cpp
void StateMachine::HandleEvent(const VisEvent& ev) {
  const Transition* t = FindGlobalTransition(ev.type);
  if (!t) t = FindStateTransition(current_state_, ev.type, ctx_, config_);
  if (!t) {
    LogUnhandled(current_state_, ev);
    return;
  }

  OnExit(current_state_);
  RunAction(t->action, ev, ctx_, config_);
  current_state_ = t->to;
  OnEnter(current_state_);
}
```

7. 与多轮语音交互目标的对应关系

- `kAckSpeaking -> kWaitingCommandSpeech`：开启首轮命令窗口
- `kResultSpeaking -> kWaitingCommandSpeech`：每轮播报后继续可说下一条命令
- 仅在 `kCommandWaitTimeout` 时回退 `kWaitingWakeup`
- `kShutdownRequested` 全局可中断并进入 `kShuttingDown`

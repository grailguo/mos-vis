mos_vis 状态机到 C++ 枚举/Transition Map 映射（v2）
1. 设计修订目标

相对 v1，v2 主要增加：

kNluTimeout
kStartRagQuery / kStartLlmChat
kUserBargeIn
kControlAsyncEventMatchesCurrent / kKeepSessionOpen
kStopCurrentTtsAndStartAsr
kDiscardStaleControlAsyncEvent
kPrepareReplyAsrRetry
kEndSessionOrRearmCommandWait

这样做后，状态机会更适合真实工程里的：

WSS 长连接异步 notify
长任务 request_id / analysis_uuid 归属
多轮语音连续交互
用户抢话打断
2. 推荐数据结构（v2）
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

  // Wake / KWS
  kWakeSpeechDetected,
  kKwsMatched,
  kKwsRejected,
  kKwsTimeout,

  // Command speech / ASR / Barge-in
  kCommandSpeechDetected,
  kCommandWaitTimeout,
  kAsrPartialResult,
  kAsrFinalResult,
  kAsrEndpointWithEmptyText,
  kAsrTimeout,
  kAsrError,
  kUserBargeIn,              // TTS 播报期间检测到用户重新说话

  // NLU
  kIntentControl,
  kIntentRag,
  kIntentLlm,
  kIntentUnknown,
  kNluError,
  kNluTimeout,

  // Control plane
  kControlSyncAckSuccess,
  kControlSyncAckFail,
  kControlSyncAckTimeout,
  kControlTransportError,
  kControlAsyncSuccess,
  kControlAsyncFail,
  kControlAsyncTimeout,
  kControlTransportDisconnected,
  kControlAsyncStaleOrMismatched,  // 收到旧 notify / 非当前任务 notify

  // RAG
  kRagSuccess,
  kRagEmpty,
  kRagTimeout,
  kRagError,

  // LLM
  kLlmSuccess,
  kLlmTimeout,
  kLlmError,

  // TTS
  kTtsPlaybackCompleted,
  kTtsPlaybackFailed,
};

enum class GuardType {
  kAlways,

  // Wake guards
  kNotInWakeCooldown,
  kInWakeCooldown,

  // ASR guards
  kAsrFinalTextNonEmpty,
  kAsrFinalTextEmpty,

  // Fallback / session policy
  kFallbackLlmEnabled,
  kFallbackLlmDisabled,
  kKeepSessionOpen,
  kNotKeepSessionOpen,

  // Async control guards
  kControlAsyncEventMatchesCurrent,
  kControlAsyncEventNotMatchesCurrent,
};

enum class ActionType {
  kNoop,

  // Boot / shutdown / recovery
  kInitDone,
  kRecordBootError,
  kRunErrorRecovery,
  kEnterShuttingDown,

  // Wake / KWS
  kOpenKwsWindow,
  kCleanupKwsWindow,
  kPlayAck,

  // Command window / session
  kEnterWaitingCommandWindow,      // 启动/重启 command_wait_timeout
  kEndSessionToWakeup,
  kEndSessionOrRearmCommandWait,   // 按策略决定：结束 session 或继续等命令

  // ASR / NLU
  kStartAsrForCommand,
  kUpdateAsrPartial,
  kSubmitAsrFinalToNlu,
  kCleanupAsrAndRearmCommandWait,
  kPrepareReplyAsrRetry,
  kPrepareReplyUnknown,

  // Control
  kSendControlRequest,
  kStartWaitingAsyncNotify,
  kPrepareReplyControlFail,
  kPrepareReplyControlTimeout,
  kPrepareReplyControlTransportError,
  kPrepareReplyControlDone,
  kDiscardStaleControlAsyncEvent,

  // RAG / LLM
  kStartRagQuery,
  kStartLlmChat,
  kPrepareReplyQueryResult,
  kPrepareReplyQueryEmpty,
  kPrepareReplyQueryFail,
  kPrepareReplyLlmResult,
  kPrepareReplyLlmFail,

  // TTS
  kPlayReply,
  kStopCurrentTtsAndStartAsr,
};

struct VisEvent {
  VisEventType type;
  std::string text;
  std::string request_id;   // 可放 request_id / analysis_uuid / task_id
};

struct Transition {
  VisState from;
  VisEventType event;
  GuardType guard;
  VisState to;
  ActionType action;
};
3. Transition Map（v2）
constexpr Transition kTransitions[] = {
  // =========================================================
  // 全局高优先级规则
  // =========================================================
  {VisState::kAny, VisEventType::kShutdownRequested, GuardType::kAlways,
   VisState::kShuttingDown, ActionType::kEnterShuttingDown},

  {VisState::kAny, VisEventType::kFatalError, GuardType::kAlways,
   VisState::kErrorRecovery, ActionType::kRunErrorRecovery},

  // 播报期间支持用户打断（barge-in）
  {VisState::kAckSpeaking, VisEventType::kUserBargeIn, GuardType::kAlways,
   VisState::kRecognizingCommand, ActionType::kStopCurrentTtsAndStartAsr},

  {VisState::kResultSpeaking, VisEventType::kUserBargeIn, GuardType::kAlways,
   VisState::kRecognizingCommand, ActionType::kStopCurrentTtsAndStartAsr},

  // WaitingControlAsync 期间如果收到旧任务/非当前任务 notify，直接丢弃且不换状态
  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncStaleOrMismatched,
   GuardType::kAlways, VisState::kWaitingControlAsync,
   ActionType::kDiscardStaleControlAsyncEvent},

  // =========================================================
  // Booting
  // =========================================================
  {VisState::kBooting, VisEventType::kBootCompleted, GuardType::kAlways,
   VisState::kWaitingWakeup, ActionType::kInitDone},

  {VisState::kBooting, VisEventType::kBootFailed, GuardType::kAlways,
   VisState::kErrorRecovery, ActionType::kRecordBootError},

  // =========================================================
  // WaitingWakeup
  // =========================================================
  {VisState::kWaitingWakeup, VisEventType::kWakeSpeechDetected, GuardType::kAlways,
   VisState::kWakeDetecting, ActionType::kOpenKwsWindow},

  // =========================================================
  // WakeDetecting
  // =========================================================
  {VisState::kWakeDetecting, VisEventType::kKwsMatched, GuardType::kNotInWakeCooldown,
   VisState::kAckSpeaking, ActionType::kPlayAck},

  {VisState::kWakeDetecting, VisEventType::kKwsMatched, GuardType::kInWakeCooldown,
   VisState::kWaitingWakeup, ActionType::kCleanupKwsWindow},

  {VisState::kWakeDetecting, VisEventType::kKwsRejected, GuardType::kAlways,
   VisState::kWaitingWakeup, ActionType::kCleanupKwsWindow},

  {VisState::kWakeDetecting, VisEventType::kKwsTimeout, GuardType::kAlways,
   VisState::kWaitingWakeup, ActionType::kCleanupKwsWindow},

  // =========================================================
  // AckSpeaking
  // =========================================================
  {VisState::kAckSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kAlways,
   VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},

  {VisState::kAckSpeaking, VisEventType::kTtsPlaybackFailed, GuardType::kAlways,
   VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},

  // =========================================================
  // WaitingCommandSpeech
  // =========================================================
  {VisState::kWaitingCommandSpeech, VisEventType::kCommandSpeechDetected, GuardType::kAlways,
   VisState::kRecognizingCommand, ActionType::kStartAsrForCommand},

  {VisState::kWaitingCommandSpeech, VisEventType::kCommandWaitTimeout, GuardType::kAlways,
   VisState::kWaitingWakeup, ActionType::kEndSessionToWakeup},

  // =========================================================
  // RecognizingCommand
  // =========================================================
  {VisState::kRecognizingCommand, VisEventType::kAsrPartialResult, GuardType::kAlways,
   VisState::kRecognizingCommand, ActionType::kUpdateAsrPartial},

  {VisState::kRecognizingCommand, VisEventType::kAsrFinalResult, GuardType::kAsrFinalTextNonEmpty,
   VisState::kUnderstandingIntent, ActionType::kSubmitAsrFinalToNlu},

  {VisState::kRecognizingCommand, VisEventType::kAsrFinalResult, GuardType::kAsrFinalTextEmpty,
   VisState::kResultSpeaking, ActionType::kPrepareReplyAsrRetry},

  {VisState::kRecognizingCommand, VisEventType::kAsrEndpointWithEmptyText, GuardType::kAlways,
   VisState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait},

  {VisState::kRecognizingCommand, VisEventType::kAsrTimeout, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyAsrRetry},

  {VisState::kRecognizingCommand, VisEventType::kAsrError, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyAsrRetry},

  // =========================================================
  // UnderstandingIntent
  // =========================================================
  {VisState::kUnderstandingIntent, VisEventType::kIntentControl, GuardType::kAlways,
   VisState::kExecutingControlSync, ActionType::kSendControlRequest},

  {VisState::kUnderstandingIntent, VisEventType::kIntentRag, GuardType::kAlways,
   VisState::kQueryingRag, ActionType::kStartRagQuery},

  {VisState::kUnderstandingIntent, VisEventType::kIntentLlm, GuardType::kAlways,
   VisState::kChattingLlm, ActionType::kStartLlmChat},

  {VisState::kUnderstandingIntent, VisEventType::kIntentUnknown, GuardType::kFallbackLlmEnabled,
   VisState::kChattingLlm, ActionType::kStartLlmChat},

  {VisState::kUnderstandingIntent, VisEventType::kIntentUnknown, GuardType::kFallbackLlmDisabled,
   VisState::kResultSpeaking, ActionType::kPrepareReplyUnknown},

  {VisState::kUnderstandingIntent, VisEventType::kNluError, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyUnknown},

  {VisState::kUnderstandingIntent, VisEventType::kNluTimeout, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyUnknown},

  // =========================================================
  // ExecutingControlSync
  // =========================================================
  {VisState::kExecutingControlSync, VisEventType::kControlSyncAckSuccess, GuardType::kAlways,
   VisState::kWaitingControlAsync, ActionType::kStartWaitingAsyncNotify},

  {VisState::kExecutingControlSync, VisEventType::kControlSyncAckFail, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlFail},

  {VisState::kExecutingControlSync, VisEventType::kControlSyncAckTimeout, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlTimeout},

  {VisState::kExecutingControlSync, VisEventType::kControlTransportError, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlTransportError},

  // =========================================================
  // WaitingControlAsync
  // 说明：只有 request_id / analysis_uuid 归属当前 pending task 的事件
  // 才应被包装成 Success/Fail 事件投递到状态机
  // =========================================================
  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncSuccess,
   GuardType::kControlAsyncEventMatchesCurrent,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlDone},

  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncFail,
   GuardType::kControlAsyncEventMatchesCurrent,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlFail},

  {VisState::kWaitingControlAsync, VisEventType::kControlAsyncTimeout, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlTimeout},

  {VisState::kWaitingControlAsync, VisEventType::kControlTransportDisconnected, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyControlTransportError},

  // =========================================================
  // QueryingRag
  // =========================================================
  {VisState::kQueryingRag, VisEventType::kRagSuccess, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyQueryResult},

  {VisState::kQueryingRag, VisEventType::kRagEmpty, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyQueryEmpty},

  {VisState::kQueryingRag, VisEventType::kRagTimeout, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyQueryFail},

  {VisState::kQueryingRag, VisEventType::kRagError, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyQueryFail},

  // =========================================================
  // ChattingLlm
  // =========================================================
  {VisState::kChattingLlm, VisEventType::kLlmSuccess, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyLlmResult},

  {VisState::kChattingLlm, VisEventType::kLlmTimeout, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyLlmFail},

  {VisState::kChattingLlm, VisEventType::kLlmError, GuardType::kAlways,
   VisState::kResultSpeaking, ActionType::kPrepareReplyLlmFail},

  // =========================================================
  // ResultSpeaking
  // 多轮策略：不是一刀切，按 guard 决定继续等命令还是回退唤醒
  // =========================================================
  {VisState::kResultSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kKeepSessionOpen,
   VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},

  {VisState::kResultSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kNotKeepSessionOpen,
   VisState::kWaitingWakeup, ActionType::kEndSessionToWakeup},

  {VisState::kResultSpeaking, VisEventType::kTtsPlaybackFailed, GuardType::kKeepSessionOpen,
   VisState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow},

  {VisState::kResultSpeaking, VisEventType::kTtsPlaybackFailed, GuardType::kNotKeepSessionOpen,
   VisState::kWaitingWakeup, ActionType::kEndSessionToWakeup},

  // =========================================================
  // ErrorRecovery
  // =========================================================
  {VisState::kErrorRecovery, VisEventType::kRecoverySucceeded, GuardType::kAlways,
   VisState::kWaitingWakeup, ActionType::kNoop},

  {VisState::kErrorRecovery, VisEventType::kRecoveryFailed, GuardType::kAlways,
   VisState::kShuttingDown, ActionType::kEnterShuttingDown},
};
4. Guard 语义（v2）
kNotInWakeCooldown
  => !ctx.InWakeCooldown()

kInWakeCooldown
  => ctx.InWakeCooldown()

kAsrFinalTextNonEmpty
  => !Trim(ev.text).empty()

kAsrFinalTextEmpty
  => Trim(ev.text).empty()

kFallbackLlmEnabled
  => config.fallback_llm_enabled == true

kFallbackLlmDisabled
  => config.fallback_llm_enabled == false

kKeepSessionOpen
  => ctx.keep_session_open == true

kNotKeepSessionOpen
  => ctx.keep_session_open == false

kControlAsyncEventMatchesCurrent
  => ev.request_id == ctx.current_control_request_id

kControlAsyncEventNotMatchesCurrent
  => ev.request_id != ctx.current_control_request_id
5. Action 语义（v2）

下面是 v2 新增和修订后最关键的 action 说明。

kStartAsrForCommand

进入命令识别阶段时：

清空上一轮 partial/final 文本
打开 command ASR 会话
停止 command wait timer
标记 ctx.session_phase = command_recognizing
kPrepareReplyAsrRetry

用于 kAsrTimeout / kAsrError / kAsrFinalResult(text 为空)：

准备简短回复，如“没听清，请再说一次”
设定 ctx.keep_session_open = true
可直接触发 reply TTS（并存模式）

这样比直接静默回 kWaitingCommandSpeech 更合理。

kStartRagQuery

必须做这几件事：

以当前 NLU 解析结果构造 query
发起异步 RAG 检索
启动 rag timeout timer
保存 ctx.current_rag_request_id
kStartLlmChat

必须做这几件事：

以当前用户文本 / 上下文构造 prompt
发起异步 LLM 请求
启动 llm timeout timer
保存 ctx.current_llm_request_id
kSendControlRequest

必须做这几件事：

基于 NLU 结果映射控制 method 和参数
调 SimpleControlEngine / WebSocketClient 发起 WSS 请求
保存 ctx.current_control_request_id
启动 sync ack timeout timer
kStartWaitingAsyncNotify

在收到控制同步 ack 后：

停掉 sync ack timeout
启动 control async completion timer
从 ack 中提取 request_id / analysis_uuid / task_id
把它写入 ctx.current_control_request_id

这一步很关键，因为后续 notify 归属要靠它。

kDiscardStaleControlAsyncEvent

用于收到旧的 / 非当前任务的 notify：

记录日志
计数 stale notify 次数
不改变当前状态
不给用户播报
kStopCurrentTtsAndStartAsr

barge-in 的核心动作：

停止当前 ack/reply TTS
关闭对应 TTS completion 回调
启动 command ASR
清理正在播报的 reply buffer
进入 kRecognizingCommand
kPrepareReplyControlDone

建议根据控制结果设置 session 策略：

对普通控制命令：ctx.keep_session_open = true
对终止型命令（如 shutdown / logout / stop session）：ctx.keep_session_open = false

这样 ResultSpeaking 出口才更灵活。

5.1 TTS 触发并存约定（新增）

v2 明确采用“并存模式”：

- `ResultSpeaking` 可以作为统一播报态，在进入态或 `OnEnter(kResultSpeaking)` 里发起 TTS。
- 某些 `Prepare*` action（如 `kPrepareReplyAsrRetry`）也允许直接发起 TTS，以减少一次状态内分派延迟。
- 为避免重复播报，必须有幂等约定：例如用 `ctx.reply_tts_started` 或 `ctx.reply_playback_token` 标识“本轮 reply 是否已启动 TTS”。
- 当 `Prepare*` 已启动 TTS 时，`ResultSpeaking` 的入口逻辑只做兜底检查，不重复 `Play`。

6. 匹配优先级（v2）

保持 v1 的顺序，但再补一条“同 event 多 guard 时按表顺序匹配”。

1. 先查找 {from = kAny, event = e.type}
2. 再查找 {from = current_state, event = e.type}
3. 对同状态同事件，按表中顺序检查 guard，取首个成立者
4. 若无匹配，记录 UnhandledEvent，保持原状态
7. 实现约定（非常重要）
7.1 HandleEvent() 禁止重入

很多 action 会触发异步请求或回调，必须规定：

action 内产生的新事件一律投递到事件队列
不允许在 action 内直接同步再次调用 HandleEvent()

推荐框架：

void StateMachine::HandleEvent(const VisEvent& ev) {
  if (in_handle_event_) {
    EnqueueEvent(ev);
    return;
  }

  ScopedFlag guard(&in_handle_event_);

  const Transition* t = FindGlobalTransition(ev.type);
  if (!t) {
    t = FindStateTransition(current_state_, ev.type, ctx_, config_);
  }
  if (!t) {
    LogUnhandled(current_state_, ev);
    return;
  }

  OnExit(current_state_);
  RunAction(t->action, ev, ctx_, config_);
  current_state_ = t->to;
  OnEnter(current_state_);
}
7.2 控制异步 notify 必须先做归属判断，再投递状态机

例如 WSS 收到 calibration_result_notify 时：

先解析 analysis_uuid
比较 analysis_uuid == ctx.current_control_request_id
相等才投递 kControlAsyncSuccess / kControlAsyncFail
不等则投递 kControlAsyncStaleOrMismatched

不要把原始 notify 直接送状态机。

7.3 进入 kQueryingRag / kChattingLlm 必须立刻启动异步任务

这就是 v2 把 kNoop 改成 kStartRagQuery / kStartLlmChat 的原因。否则状态进入了，请求却没发出去。

8. 推荐的 Context 字段补充
struct VisContext {
  std::chrono::steady_clock::time_point last_kws_timestamp;
  std::string asr_partial_text;
  std::string asr_final_text;

  std::string current_control_request_id;
  std::string current_rag_request_id;
  std::string current_llm_request_id;

  bool keep_session_open = true;
  bool fallback_llm_enabled = false;

  // 运行中资源句柄 / timer handle / request handle
  // ...
};

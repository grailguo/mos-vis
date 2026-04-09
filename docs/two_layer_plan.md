# Two-Layer State Machine Refactor Plan

参见 [state_machine_v3.md](./state_machine_v3.md) 获取最新状态机设计文档。

## Summary

本方案以现有 `SessionState + stage` 流水线为主骨架，不做全量重写。  
在热点环节引入局部子状态机（transition map + guard）与轻量事件总线，降低分支复杂度并对齐 v2 文档目标。

## Layer Design

### Layer-1（主骨架）

- 继续使用 `SessionState` 作为全局会话主状态。
- 继续使用 `PipelineScheduler` 和既有 stage 执行顺序。
- stage 负责 I/O（音频、引擎调用、网络轮询）与基础状态推进。

### Layer-2（局部子状态机）

- 子状态机仅在热点 stage 内生效，不接管全局生命周期。
- 统一模型：`Event -> Guard -> Transition -> Action`。
- 轻量事件总线仅服务这些子状态机，不引入全局 event-loop 重构。

## Hotspot Scope

### 1) Wake 子状态机（KWS）

- 事件：`KwsMatched/KwsRejected/KwsTimeout`
- Guard：`InCooldown/NotInCooldown`
- 行为：命中唤醒、cooldown 忽略、窗口清理

### 2) ASR 子状态机（ASR）

- 事件：`AsrFinalNonEmpty/AsrFinalEmpty/AsrTimeout/AsrError`
- 行为：
  - `AsrFinalNonEmpty -> Recognizing`
  - `AsrFinalEmpty/Timeout/Error -> “没听清，请再说一次”` 并回可继续说命令路径

### 3) Reply 子状态机（TTS/ResultSpeaking）

- 事件：`TtsPlaybackCompleted/TtsPlaybackFailed/UserBargeIn`
- Guard：`keep_session_open`
- 行为：继续命令窗口或结束回待唤醒；支持 barge-in 入口

## Interfaces & Context

- `SessionContext` 增加：
  - `keep_session_open`
  - `current_control_request_id`
  - `reply_playback_token`
  - `reply_tts_started`
  - `local_events`（Wake/ASR/Reply 事件队列）
  - `subsm_state`（Wake/ASR/Reply 子状态机状态）
- `ControlResult` 增加：
  - `request_id`
  - `task_id`

## Control Async Filtering

- 在 `ControlNotificationStage` 中对 notify 进行归属判断：
  - `notify.request_id == ctx.current_control_request_id` 才播报
  - 不匹配则记 stale 日志并丢弃

## Observability

- 每个子状态机转移输出结构化日志：`from/event/guard/to/action`。
- 关键分支记录用户可读日志（cooldown 忽略、stale notify 丢弃、barge-in 触发）。

## Validation Checklist

- 多轮交互：唤醒后连续命令，直至超时回待唤醒
- ASR 空文本：走“请重试”播报
- Reply 出口：`keep_session_open` true/false 双分支
- 控制异步通知：匹配与 stale 丢弃
- barge-in：播报期间触发事件进入识别链路


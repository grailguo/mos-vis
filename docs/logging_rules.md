# mos_vis 日志规则（实时离线语音交互系统）

## 1. 目标

本规则用于统一 `C++ + spdlog` 日志实践，满足两类场景：

- 调试跟踪：复现语音链路问题（KWS/VAD/ASR/NLU/Control/TTS）
- 日常排障：快速定位“为什么没唤醒/没识别/没执行/没播报”

本项目当前日志框架：`GetLogger()` + `spdlog`，默认 `info`，`verbose` 下 `debug`。

## 2. 级别定义（必须遵守）

- `ERROR`：功能失败、影响当前会话主流程，需要人工关注  
  例：引擎调用失败、阶段处理失败、状态不一致导致流程中断
- `WARN`：可恢复异常，不一定失败，但有风险  
  例：超时降级、重试触发、收到 stale notify、空结果回退
- `INFO`：关键业务里程碑，默认线上可见  
  例：会话开始/结束、唤醒命中、最终识别文本、控制执行结果、播报结果
- `DEBUG`：高频或细粒度诊断，仅调试开启  
  例：子状态机转移细节、音频统计、局部 guard 计算值
- `TRACE`：极高频底层数据，默认禁用  
  例：逐帧窗口明细、每 chunk 全量指标

约束：

- 禁止把“成功路径高频事件”放在 `INFO`（会淹没关键事件）
- 禁止把“失败路径”只打 `DEBUG`

## 3. 日志格式与字段规范

## 3.1 统一输出格式（建议）

保持现有 pattern 基础上，业务消息统一为 key-value 风格，便于 grep：

`module=<M> event=<E> session=<S> state=<ST> req=<RID> result=<R> detail=<...>`

## 3.2 必带上下文字段

- `module`：模块名，如 `KwsStage/AsrStage/TtsStage/Control`
- `event`：业务事件名，如 `kws_hit/asr_final/control_notify`
- `session`：会话标识（建议新增 `session_id`）
- `state`：主状态（`SessionState`）
- `subsm`：子状态机名与状态（如 `wake/asr/reply`，仅热点流程）
- `req`：请求归属 ID（control 的 `request_id/analysis_uuid/task_id`）

规则：

- 有 `request_id` 的日志必须带 `req`
- 状态转移日志必须带 `from/to/event/action`

## 4. 热点链路必打点（最小集合）

## 4.1 唤醒链路（VAD1/KWS）

- `INFO`：唤醒命中（keyword、是否 cooldown 命中）
- `WARN`：KWS reset 失败、关键窗口异常
- `DEBUG`：wake subsm 转移（from/event/guard/to/action）

## 4.2 命令识别链路（VAD2/ASR）

- `INFO`：语音开始、语音结束、ASR final（仅 final）
- `WARN`：`AsrTimeout/AsrError/AsrFinalEmpty` 及回退动作
- `DEBUG`：partial 文本变化、ASR subsm 转移

约束：

- partial 文本必须 `DEBUG`，禁止 `INFO`
- final 文本可 `INFO`，但需脱敏策略（见第 7 节）

## 4.3 语义与执行链路（NLU/Control）

- `INFO`：intent 路由结果、control execute begin/done
- `WARN`：NLU 推理失败、control retry、stale notify 丢弃
- `ERROR`：control 请求失败且不可恢复

## 4.4 播报与会话出口（TTS/Reply）

- `INFO`：播报开始/结束、会话出口（继续等命令 or 回待唤醒）
- `WARN`：TTS 失败、barge-in 降级处理
- `DEBUG`：reply subsm 转移与 `keep_session_open` 判定

## 5. 高频日志治理（必须）

为防实时链路被日志拖慢：

- 高频循环（音频 chunk/window）默认只打 `DEBUG/TRACE`
- 同类告警做限频：建议“首条 + 每 N 次 + 恢复时一条”
- 重复错误附带计数器：`count=<n>`，避免刷屏
- `INFO` 默认不打印每帧统计

推荐限频阈值：

- VAD/KWS 空转日志：每 1s 或每 50 tick 一条
- 重复网络告警：每 5s 一条

## 6. 状态机日志规则（两层模型）

## 6.1 主状态（Layer-1）

仅在主状态变化时打 `INFO`：

`event=state_transition layer=main from=<...> to=<...> reason=<...>`

## 6.2 子状态机（Layer-2）

默认 `DEBUG`：

`event=subsm_transition subsm=<wake|asr|reply> from=<...> ev=<...> guard=<...> to=<...> action=<...>`

若触发降级/回退（timeout/error/stale）则升级到 `WARN`。

## 7. 安全与隐私

- ASR final 文本、NLU 原文、WS payload 可能含敏感信息  
  默认规则：`INFO` 可打印摘要；全量文本仅 `DEBUG` 且仅调试环境
- 设备/用户标识需脱敏（如仅保留前后缀）
- 禁止打印密钥、token、完整授权报文

## 8. 错误日志结构（统一模板）

`module=<M> event=<E> err_code=<C> err=<MSG> state=<ST> session=<S> req=<RID> action=<NEXT>`

其中 `action` 必填，用于标记后续处理：

- `retry`
- `fallback`
- `drop`
- `abort`

## 9. 日常排障查询手册（grep）

- 看单会话主线：`session=<S>`
- 看状态跳转：`event=state_transition|event=subsm_transition`
- 看控制归属问题：`event=control_notify|stale control notify dropped`
- 看识别失败原因：`AsrTimeout|AsrError|AsrFinalEmpty|NLU infer failed`
- 看播报问题：`TTS task failed|event=reply`

## 10. 落地建议（本项目）

短期（立即）：

- 新增 `session_id` 并贯穿所有 stage 日志
- 对 `INFO` 级别做“关键里程碑收敛”，把高频信息降为 `DEBUG`
- 统一 `control` 日志补齐 `req=<request_id>`

中期（1-2 迭代）：

- 增加统一日志辅助函数（封装字段拼接，减少手写格式漂移）
- 引入限频工具（按模块/事件）
- 增加 `trace` 开关，默认关闭

长期：

- 输出结构化日志（JSON sink）用于自动聚合与告警
- 建立“错误码字典 + runbook”对照

## 11. 验收标准

- 默认 `info` 模式下，单次完整交互日志可在 200 行内看清主链路
- 任一失败场景（未唤醒/未识别/未执行/未播报）可在 5 分钟内定位到责任模块
- 连续运行下日志不因高频 debug 导致明显性能退化

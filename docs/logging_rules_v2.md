mos_vis 日志规则（v2，实时离线语音交互系统）
1. 目标

本规则用于统一 C++ + spdlog 日志实践，满足三类核心场景：

开发调试：复现语音链路问题（KWS/VAD/ASR/NLU/Control/TTS）
性能分析：定位“慢在哪”（延迟/排队/模型耗时）
日常排障：5 分钟内定位“为什么没唤醒/没识别/没执行/没播报”

本项目日志框架：GetLogger() + spdlog
默认级别：info，调试模式：debug。

2. 日志级别定义（必须遵守）
级别	含义
ERROR	功能失败，影响当前会话主流程
WARN	可恢复异常，但存在风险
INFO	关键业务里程碑（默认线上可见）
DEBUG	高频或细粒度诊断
TRACE	极高频底层数据（默认关闭）
强约束
❌ 禁止把“成功路径高频事件”放在 INFO
❌ 禁止把“失败路径”只打 DEBUG
❌ 禁止在实时链路打未限频的高频 INFO
3. 日志格式与字段规范
3.1 统一输出格式（强烈建议）

统一 key-value 风格：

module=<M> event=<E> session=<S> turn=<T> state=<ST> req=<RID> cost_ms=<C> result=<R> detail=<...>
3.2 必带字段（强制，按事件范围）
字段	说明
module	模块名（KwsStage/AsrStage/TtsStage/Control 等）
event	业务事件名（标准化，见下）
session	会话 ID（会话内事件必须）
turn	当前轮次（会话内对话事件必须）
state	主状态（状态机相关事件必须）
req	请求 ID（control / async 任务必须）
规则
有 control 请求 → 必须带 req
状态转移 → 必须带 from/to/event/action
多轮对话事件 → 必须带 turn

事件范围定义（用于避免“强制字段过宽”）：

- 系统级事件（如 `system_boot`/设备初始化）：必须 `module,event`；可不带 `session,turn`
- 会话级事件（如 `session_start/session_end/state_transition`）：必须 `module,event,session`；`turn` 可选
- 对话轮次事件（如 `asr_* / intent_route / tts_* / control_*`）：必须 `module,event,session,turn`
3.3 延迟/耗时字段（新增，强制）

热点链路必须带至少一个：

字段	含义
cost_ms	当前步骤耗时
latency_ms	从触发到结果延迟
queue_ms	排队耗时
audio_ms	语音片段长度
ack_ms	control 请求到 ack
notify_wait_ms	ack → notify
total_ms	整轮耗时
3.4 多轮会话字段（新增）
字段	说明
session	会话 ID
turn	第几轮命令
utt	可选，语音片段编号
4. 标准事件命名（强制规范）

统一使用 snake_case：

唤醒
kws_hit
kws_reject
kws_timeout
ASR
asr_start
asr_partial
asr_final
asr_timeout
asr_error
asr_final_empty
NLU
intent_route
nlu_error
nlu_timeout
Control
control_send
control_ack
control_async_wait_begin
control_notify
control_done
control_timeout
control_stale_notify_drop
control_transport_disconnect
RAG / LLM
rag_query
rag_result
rag_timeout
llm_request
llm_result
llm_timeout
TTS
tts_start
tts_done
tts_fail
tts_barge_in
Session
session_start
session_end
state_transition
subsm_transition
system_boot
audio_device_open
audio_overrun
audio_silence_too_long
audio_sr_mismatch
kws_reset_failed
control_retry
5. 热点链路必打点（最小集合）
5.1 唤醒链路

INFO：

event=kws_hit keyword=<kw> cooldown=<0/1>

WARN：

kws_reset_failed

DEBUG：

event=subsm_transition subsm=wake from=<...> ev=<...> guard=<...> to=<...> action=<...>
5.2 ASR 链路

INFO：

asr_start
asr_final text=<masked> cost_ms=...

WARN：

asr_timeout / asr_error / asr_final_empty

DEBUG：

asr_partial text=...
5.3 NLU / Control 链路（增强）

INFO：

intent_route intent=<...>
control_send req=<id>
control_ack req=<id> ack_ms=...
control_done req=<id> notify_wait_ms=...

WARN：

control_retry
control_stale_notify_drop

ERROR：

control_failed action=abort
5.4 TTS / Reply

INFO：

tts_start
tts_done cost_ms=...
session_end reason=<...>

WARN：

tts_fail
tts_barge_in
6. 高频日志治理（必须）
高频循环 → 仅 DEBUG/TRACE
同类告警 → 限频（首条 + 每 N 次 + 恢复）
重复错误 → count=<n>
INFO 禁止逐帧统计

推荐：

类型	限频
VAD/KWS	每 1s
网络错误	每 5s
7. 状态机日志规则（两层模型）
7.1 主状态（INFO）
event=state_transition layer=main from=<...> to=<...> reason=<...>
7.2 子状态机（DEBUG）
event=subsm_transition subsm=<...> from=<...> ev=<...> guard=<...> to=<...> action=<...>

异常升级：

timeout/error → WARN
8. 控制异步归属规则（新增关键）

收到 notify 时：

必须先校验 req
匹配 → 进入状态机
不匹配 → 打：
event=control_stale_notify_drop req=<rid>
9. 音频健康度日志（新增）
必打：

INFO：

audio_device_open sr=16000 ch=1 buf=512

WARN：

audio_overrun
audio_silence_too_long
audio_sr_mismatch

DEBUG（限频）：

rms=<...> peak=<...> clip=<0/1>
10. 启动配置快照（新增）

系统启动时：

event=system_boot version=<...> config=<...> model=<...> wake_cooldown_ms=... command_timeout_ms=...
11. 安全与隐私（强化）
内容	规则
token	❌禁止打印
user_id	部分脱敏
ASR final	INFO 打摘要（前 N 字）
payload	不打印完整 JSON
路径	生产环境默认 basename；调试环境（debug）可打印完整路径

11.1 规则优先级（消除冲突）

当“配置快照可观测性”与“隐私最小暴露”冲突时，优先级如下：

1. 安全与隐私规则优先于可观测性
2. 生产环境路径默认 `basename`
3. 仅在调试模式（debug）允许打印完整 `config` 路径
12. 错误日志模板（统一）
module=<M> event=<E> err_code=<C> err=<MSG>
state=<ST> session=<S> turn=<T> req=<RID>
action=<retry|fallback|drop|abort>
reason=<root_cause>
13. 排障查询手册（增强版）
单会话
grep "session=<S>"
单轮
grep "session=<S> turn=<T>"
状态流
grep "event=state_transition"
控制问题
grep "control_"
ASR 问题
grep "asr_"
TTS 问题
grep "tts_"
性能问题
grep "cost_ms\|latency_ms"
14. 落地建议
短期
加 session + turn + req
收敛 INFO
控制链路补齐事件
中期
封装日志宏（避免字段遗漏）
引入限频工具
引入 context logger
长期
JSON structured log
ELK / Loki / Grafana
error code + runbook
15. 验收标准（升级版）
单轮交互日志 ≤ 150 行
任一问题 5 分钟定位
可分析：
慢在哪
错在哪
属于哪个模块
日志不会影响实时性能

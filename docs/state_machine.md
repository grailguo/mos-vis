mos_vis 语音交互系统状态机定义文档 v1

> **注意**：本文档为历史版本，已被 [state_machine_v3.md](./state_machine_v3.md) 替代。请参考 v3 文档获取最新设计。

1. 目标

本文档定义 mos_vis_server 在运行时的核心业务状态机，用于管理语音交互系统从待唤醒、唤醒确认、命令采集、语音识别、语义理解、命令执行、结果播报到回退/恢复的完整流程。

该状态机用于协调以下模块：

AudioCapture
AudioRingBuffer
VadEngine
SherpaKwsEngine
SherpaAsrEngine
SimpleNluEngine
SimpleControlEngine
StubRagEngine
StubLlmEngine
TtsEngine
2. 设计原则
2.1 状态机只管理业务阶段

状态机负责管理“系统当前处于什么业务阶段”，不直接等同于某个底层引擎线程是否在运行。

例如：

WaitingWakeup 是业务状态
vad1_、kws_ 是否运行，是该状态下的处理策略
2.2 Pipeline 负责连续数据流

音频采集和缓存是连续进行的，状态机只决定：

哪些模块当前允许消费音频
哪些模块当前应忽略输入
哪些结果可以触发状态转移
2.3 明确 TTS 播放态

TTS 播放期间必须有显式状态，避免：

播报语音被 KWS 误识别为唤醒词
播报内容被 ASR 误识别为用户命令
状态跳转与播报动作交叉
2.4 已唤醒会话有超时窗口

KWS 成功后，系统进入“已唤醒命令窗口”。
在该窗口内，允许用户连续发出命令；若超时无输入，则退回待唤醒状态。

2.5 控制命令支持同步确认与异步完成通知

仪器控制类命令采用两阶段反馈：

sync ack：服务端已接收/受理命令
async notify：耗时执行完成/失败通知
3. 总体状态图

主状态流如下：

Booting
→ WaitingWakeup
→ WakeDetecting
→ AckSpeaking
→ WaitingCommandSpeech
→ RecognizingCommand
→ UnderstandingIntent
→ ExecutingControlSync / WaitingControlAsync / QueryingRag / ChattingLlm
→ ResultSpeaking
→ WaitingCommandSpeech 或 WaitingWakeup

异常情况下进入：

ErrorRecovery
或 ShuttingDown
4. 状态定义
4.1 Booting
含义

系统启动、初始化资源、加载配置、构建模块连接关系。

进入动作
加载配置文件
初始化日志系统
初始化 AudioCapture
初始化 AudioRingBuffer
初始化 VadEngine(vad1_, vad2_)
初始化 SherpaKwsEngine
初始化 SherpaAsrEngine
初始化 SimpleNluEngine
初始化 SimpleControlEngine
初始化 Rag/Llm/Tts
建立 WebSocket 或其他控制连接
启动音频采集线程
允许事件
BootCompleted
BootFailed
转移
BootCompleted → WaitingWakeup
BootFailed → ErrorRecovery
4.2 WaitingWakeup
含义

系统处于待唤醒状态，等待用户说出唤醒词。

处理策略
AudioCapture 持续采集音频，写入 AudioRingBuffer
vad1_ 对 ring buffer 中音频进行前置语音活动检测
仅当 vad1_ 判定存在疑似语音段时，允许 SherpaKwsEngine 进入唤醒词检测窗口
此状态下不启动命令 ASR/NLU 流程
进入动作
清理上一轮会话上下文
清理临时 ASR 文本
清理 NLU 中间状态
启用 vad1_
禁止 vad2_
禁止命令 ASR
禁止命令 NLU
禁止控制执行结果消费
重置 wake cooldown 检查条件
持续动作
vad1_ 持续分析音频
若检测到连续满足阈值的语音段，则触发 WakeSpeechDetected
允许事件
WakeSpeechDetected
ShutdownRequested
FatalError
转移
WakeSpeechDetected → WakeDetecting
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
4.3 WakeDetecting
含义

对当前疑似语音段进行唤醒词检测。

处理策略
使用 SherpaKwsEngine 对当前语音窗口进行检测
该状态只负责判断是否命中唤醒词，不进入命令识别
进入动作
锁定当前 KWS 检测窗口
启用 SherpaKwsEngine
暂停继续扩张检测窗口的策略，防止无限等待
记录检测开始时间
允许事件
KwsMatched
KwsRejected
KwsTimeout
ShutdownRequested
FatalError
转移
KwsMatched 且 非 cooldown → AckSpeaking
KwsMatched 且 cooldown 命中 → WaitingWakeup
KwsRejected → WaitingWakeup
KwsTimeout → WaitingWakeup
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
说明
命中唤醒词后，应进入 wake cooldown
cooldown 内新的 KWS 结果忽略
若系统已处于已唤醒相关状态，则新的 KWS 结果默认忽略
4.4 AckSpeaking
含义

系统已成功被唤醒，正在播报唤醒确认语音。

例如：

“我在，请讲”
“在呢，请下达指令”
“收到，请说”
进入动作
禁止 vad1_
禁止 vad2_
禁止 KWS
禁止命令 ASR
调用 TtsEngine 播放 ACK 语音
允许事件
TtsPlaybackCompleted
TtsPlaybackFailed
ShutdownRequested
FatalError
转移
TtsPlaybackCompleted → WaitingCommandSpeech
TtsPlaybackFailed → WaitingCommandSpeech
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
说明

第一版建议 不支持 barge-in。
即 ACK 播放期间，不接受用户打断输入。

4.5 WaitingCommandSpeech
含义

系统处于“已唤醒，等待用户说命令”的阶段。

处理策略
使用 vad2_ 对音频进行命令起始检测
启动“命令等待超时计时器”，默认 15 秒
若在 15 秒内未检测到用户开始讲话，则退回 WaitingWakeup
进入动作
启用 vad2_
禁止 vad1_
禁止 KWS
禁止命令 ASR 的正式识别
重启 command_wait_timeout = 15s（每次进入该状态均执行）
持续动作
vad2_ 持续分析音频
若发现满足阈值的语音起始段，则触发 CommandSpeechDetected
允许事件
CommandSpeechDetected
CommandWaitTimeout
ShutdownRequested
FatalError
转移
CommandSpeechDetected → RecognizingCommand
CommandWaitTimeout → WaitingWakeup
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
4.6 RecognizingCommand
含义

系统正在识别用户命令语音。

处理策略
使用 SherpaAsrEngine 做流式语音识别
接受 partial result
接受 final result
接受 endpoint
若最终无有效文本，则返回 WaitingCommandSpeech
进入动作
停止 WaitingCommandSpeech 的 15 秒计时器
禁止 vad2_ 触发新的命令起始事件
启动流式 ASR
创建当前轮命令的临时识别上下文
持续动作
持续消费 ring buffer 音频
更新 partial text
检查 endpoint 条件
允许事件
AsrPartialResult(text)
AsrFinalResult(text)
AsrEndpointWithEmptyText
AsrTimeout
AsrError
ShutdownRequested
FatalError
转移
AsrPartialResult(text) → 保持当前状态
AsrFinalResult(text) 且 text 非空 → UnderstandingIntent
AsrEndpointWithEmptyText → WaitingCommandSpeech
AsrTimeout → WaitingCommandSpeech
AsrError → WaitingCommandSpeech
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
说明

AsrFinalResult(text) 触发后，应将该文本作为本轮命令输入传递给 NLU。

4.7 UnderstandingIntent
含义

对 ASR 产生的最终文本做语义理解与路由决策。

输入
final_text
进入动作
调用 SimpleNluEngine
根据规则或解析结果输出意图类型和结构化槽位
允许事件
IntentControl(parsed_command)
IntentRag(query)
IntentLlm(query)
IntentUnknown
NluError
ShutdownRequested
FatalError
转移
IntentControl(parsed_command) → ExecutingControlSync
IntentRag(query) → QueryingRag
IntentLlm(query) → ChattingLlm
IntentUnknown：
若 fallback_llm = true → ChattingLlm
否则 → ResultSpeaking
NluError → ResultSpeaking
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
说明

对于 IntentUnknown，推荐准备固定回复文本：

“未理解你的指令，请再说一次。”
4.8 ExecutingControlSync
含义

向仪器控制服务发送控制命令，并等待同步确认。

输入
结构化控制命令
唯一 request_id
进入动作
构建控制报文
通过 SimpleControlEngine 发送请求
启动 sync_ack_timeout
进入“等待同步确认”阶段
允许事件
ControlSyncAckSuccess
ControlSyncAckFail
ControlSyncAckTimeout
ControlTransportError
ShutdownRequested
FatalError
转移
ControlSyncAckSuccess → WaitingControlAsync
ControlSyncAckFail → ResultSpeaking
ControlSyncAckTimeout → ResultSpeaking
ControlTransportError → ResultSpeaking
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
播报建议
sync ack success 时，不一定立即播报
也可以播报：“开始执行 xxxx”
若播报，则在进入 WaitingControlAsync 前后统一处理，避免状态交叉
4.9 WaitingControlAsync
含义

控制命令已被服务端受理，正在等待异步完成通知。

典型用于：

start_calibration
start_analysis

这类需要 5~10 分钟才能完成的耗时操作。

进入动作
记录当前 request_id
启动 async_notify_timeout
订阅/匹配对应异步通知
允许事件
ControlAsyncSuccess
ControlAsyncFail
ControlAsyncTimeout
ControlTransportDisconnected
ShutdownRequested
FatalError
转移
ControlAsyncSuccess → ResultSpeaking
ControlAsyncFail → ResultSpeaking
ControlAsyncTimeout → ResultSpeaking
ControlTransportDisconnected → ResultSpeaking
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
说明

必须按 request_id 匹配异步返回，避免旧任务消息污染当前任务状态。

4.10 QueryingRag
含义

执行信息查询类任务。

进入动作
将用户文本或 NLU 生成的 query 发送给 StubRagEngine
允许事件
RagSuccess(answer_text)
RagEmpty
RagTimeout
RagError
ShutdownRequested
FatalError
转移
RagSuccess(answer_text) → ResultSpeaking
RagEmpty → ResultSpeaking
RagTimeout → ResultSpeaking
RagError → ResultSpeaking
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
播报建议
RagEmpty → “未找到相关信息”
RagTimeout / RagError → “查询失败，请稍后重试”
4.11 ChattingLlm
含义

执行开放式对话或 LLM 兜底任务。

进入动作
将 query 发送给 StubLlmEngine
允许事件
LlmSuccess(answer_text)
LlmTimeout
LlmError
ShutdownRequested
FatalError
转移
LlmSuccess(answer_text) → ResultSpeaking
LlmTimeout → ResultSpeaking
LlmError → ResultSpeaking
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
播报建议
LlmTimeout / LlmError → “当前无法回答，请稍后再试”
4.12 ResultSpeaking
含义

播报控制结果、查询结果或错误提示。

输入
一段最终待播报文本 reply_text
进入动作
禁止 vad1_
禁止 vad2_
禁止 KWS
禁止命令 ASR
调用 TtsEngine 播放 reply_text
允许事件
TtsPlaybackCompleted
TtsPlaybackFailed
ShutdownRequested
FatalError
转移
TtsPlaybackCompleted → WaitingCommandSpeech
TtsPlaybackFailed → WaitingCommandSpeech
ShutdownRequested → ShuttingDown
FatalError → ErrorRecovery
说明

这里统一处理以下播报场景：

控制开始
控制完成
控制失败
查询结果
LLM 回答
未理解
超时
异常提示
4.13 ErrorRecovery
含义

处理不可忽略的错误，执行必要清理与恢复。

进入动作
停止当前识别/播报/控制等待
清理临时上下文
重置状态机内部变量
记录错误日志
视错误级别决定是否重建模块
允许事件
RecoverySucceeded
RecoveryFailed
ShutdownRequested
转移
RecoverySucceeded → WaitingWakeup
RecoveryFailed → ShuttingDown
ShutdownRequested → ShuttingDown
4.14 ShuttingDown
含义

系统进入显式停机终态，执行资源释放并停止事件处理。

进入动作
停止状态机事件消费
停止识别/播报/控制相关活动
释放音频与网络等运行期资源
记录停机原因与最终状态
说明

该状态为终态，不再转移到其他业务状态。
5. 事件定义

下面给出推荐事件枚举。

5.1 生命周期事件
BootCompleted
BootFailed
ShutdownRequested
FatalError
RecoverySucceeded
RecoveryFailed
5.2 唤醒相关事件
WakeSpeechDetected
KwsMatched
KwsRejected
KwsTimeout
5.3 命令等待相关事件
CommandSpeechDetected
CommandWaitTimeout
5.4 ASR 相关事件
AsrPartialResult(text)
AsrFinalResult(text)
AsrEndpointWithEmptyText
AsrTimeout
AsrError
5.5 NLU 相关事件
IntentControl(parsed_command)
IntentRag(query)
IntentLlm(query)
IntentUnknown
NluError
5.6 Control 相关事件
ControlSyncAckSuccess
ControlSyncAckFail
ControlSyncAckTimeout
ControlTransportError
ControlAsyncSuccess
ControlAsyncFail
ControlAsyncTimeout
ControlTransportDisconnected
5.7 RAG / LLM 相关事件
RagSuccess(answer_text)
RagEmpty
RagTimeout
RagError
LlmSuccess(answer_text)
LlmTimeout
LlmError
5.8 TTS 相关事件
TtsPlaybackCompleted
TtsPlaybackFailed
6. 关键上下文数据定义

建议状态机维护一个会话上下文 SessionContext。

6.1 推荐字段
session_id
request_id
wake_timestamp
last_kws_timestamp
current_state
asr_partial_text
asr_final_text
nlu_intent
nlu_slots
reply_text
control_method
sync_ack_received
async_notify_received
command_wait_deadline
sync_ack_deadline
async_notify_deadline
6.2 用途

用于：

跨状态传参
日志追踪
request_id 匹配
超时控制
错误恢复
7. 全局策略定义
7.1 音频采集始终持续

AudioCapture 与 AudioRingBuffer 原则上持续工作，不因状态切换而频繁启停。

原因：

降低设备重新打开/关闭开销
保持音频时序连续
避免切换抖动
7.2 KWS 去抖动策略
规则
同一次成功唤醒后，在 wake_cooldown_ms 内忽略新的 KWS 命中
已处于如下状态时忽略新 KWS：
AckSpeaking
WaitingCommandSpeech
RecognizingCommand
UnderstandingIntent
ExecutingControlSync
WaitingControlAsync
QueryingRag
ChattingLlm
ResultSpeaking
推荐参数
wake_cooldown_ms = 1500 ~ 3000
7.3 TTS 自监听抑制策略

第一版建议：

TTS 播放期间，禁止 KWS / ASR / vad2 的有效触发
不支持用户打断播报
若后续需要支持 barge-in，再引入 AEC / 回采抑制策略
7.4 已唤醒会话窗口策略
WaitingCommandSpeech 启动 15 秒超时
用户开始说话后停止该计时器
一轮命令处理结束并播报完成后，再重新进入 WaitingCommandSpeech
每次进入 WaitingCommandSpeech（包括来自 AckSpeaking / ResultSpeaking / RecognizingCommand 回流）都必须重启该计时器
如果 15 秒内无新命令，则回退 WaitingWakeup
7.5 控制命令互斥策略

第一版建议：

同一时刻只允许一个控制命令处于执行中
若已有控制命令在 WaitingControlAsync，新的控制类命令默认拒绝或提示“当前任务执行中”
7.6 异步通知匹配策略

必须使用 request_id 进行严格匹配：

只接收属于当前命令的 async notify
忽略历史残留 notify
忽略 method 不匹配或 request_id 不匹配的通知
8. 状态转移表

下面给出精简版状态转移表。

当前状态	事件	条件	下一个状态	动作
Booting	BootCompleted	-	WaitingWakeup	初始化完成
Booting	BootFailed	-	ErrorRecovery	记录错误
WaitingWakeup	WakeSpeechDetected	-	WakeDetecting	打开 KWS 检测窗口
WakeDetecting	KwsMatched	非 cooldown	AckSpeaking	播放 ACK
WakeDetecting	KwsMatched	处于 cooldown	WaitingWakeup	忽略命中并清理 KWS 窗口
WakeDetecting	KwsRejected	-	WaitingWakeup	清理 KWS 窗口
WakeDetecting	KwsTimeout	-	WaitingWakeup	清理 KWS 窗口
AckSpeaking	TtsPlaybackCompleted	-	WaitingCommandSpeech	启动 15 秒计时
AckSpeaking	TtsPlaybackFailed	-	WaitingCommandSpeech	启动 15 秒计时
WaitingCommandSpeech	CommandSpeechDetected	-	RecognizingCommand	启动 ASR
WaitingCommandSpeech	CommandWaitTimeout	-	WaitingWakeup	结束会话
RecognizingCommand	AsrPartialResult	-	RecognizingCommand	更新 partial
RecognizingCommand	AsrFinalResult(text)	text非空	UnderstandingIntent	提交文本
RecognizingCommand	AsrEndpointWithEmptyText	-	WaitingCommandSpeech	继续等待命令并重启 15 秒计时
RecognizingCommand	AsrTimeout	-	WaitingCommandSpeech	清理 ASR 并重启 15 秒计时
RecognizingCommand	AsrError	-	WaitingCommandSpeech	清理 ASR 并重启 15 秒计时
UnderstandingIntent	IntentControl	-	ExecutingControlSync	发送控制请求
UnderstandingIntent	IntentRag	-	QueryingRag	发起查询
UnderstandingIntent	IntentLlm	-	ChattingLlm	发起对话
UnderstandingIntent	IntentUnknown	fallback_llm=true	ChattingLlm	LLM 兜底
UnderstandingIntent	IntentUnknown	fallback_llm=false	ResultSpeaking	播报未理解
ExecutingControlSync	ControlSyncAckSuccess	-	WaitingControlAsync	等待 async
ExecutingControlSync	ControlSyncAckFail	-	ResultSpeaking	播报失败
ExecutingControlSync	ControlSyncAckTimeout	-	ResultSpeaking	播报超时
ExecutingControlSync	ControlTransportError	-	ResultSpeaking	播报连接异常
WaitingControlAsync	ControlAsyncSuccess	-	ResultSpeaking	播报完成
WaitingControlAsync	ControlAsyncFail	-	ResultSpeaking	播报失败
WaitingControlAsync	ControlAsyncTimeout	-	ResultSpeaking	播报执行超时
WaitingControlAsync	ControlTransportDisconnected	-	ResultSpeaking	播报连接中断
QueryingRag	RagSuccess	-	ResultSpeaking	播报查询结果
QueryingRag	RagEmpty	-	ResultSpeaking	播报未找到
QueryingRag	RagTimeout	-	ResultSpeaking	播报查询超时
QueryingRag	RagError	-	ResultSpeaking	播报查询失败
ChattingLlm	LlmSuccess	-	ResultSpeaking	播报回答
ChattingLlm	LlmTimeout	-	ResultSpeaking	播报回答超时
ChattingLlm	LlmError	-	ResultSpeaking	播报回答失败
ResultSpeaking	TtsPlaybackCompleted	-	WaitingCommandSpeech	再次进入命令窗口并重启 15 秒计时
ResultSpeaking	TtsPlaybackFailed	-	WaitingCommandSpeech	再次进入命令窗口并重启 15 秒计时
任意状态	ShutdownRequested	-	ShuttingDown	执行停机清理并停止状态机
任意状态	FatalError	-	ErrorRecovery	执行恢复
ErrorRecovery	RecoverySucceeded	-	WaitingWakeup	回到待唤醒
ErrorRecovery	RecoveryFailed	-	ShuttingDown	停机等待外部拉起
9. 与 Pipeline 的协同规则
9.1 推荐职责划分
Pipeline 层负责
音频采集
ring buffer 写入
vad/kws/asr 的数据消费
tts 播放
websocket 收发
各模块线程生命周期管理
StateMachine 层负责
当前业务状态
哪些模块当前有权触发事件
超时管理
事件分发
转移决策
会话上下文维护
9.2 推荐实现方式

建议采用：

StateMachine::HandleEvent(const Event&)
OnEnter(State)
OnExit(State)

配合一个统一事件总线或线程安全队列：

Pipeline 模块产出事件
StateMachine 消费事件并转移状态
10. 推荐配置项
10.1 唤醒相关
wake_cooldown_ms
vad1_min_speech_ms
kws_window_ms
kws_score_threshold
10.2 命令采集相关
command_wait_timeout_ms = 15000
vad2_min_speech_ms
asr_endpoint_silence_ms
asr_max_utterance_ms
10.3 控制相关
control_sync_timeout_ms
control_async_timeout_ms
control_ws_reconnect_enabled
10.4 语义相关
fallback_llm
nlu_rule_strict_mode
10.5 TTS 相关
tts_ack_text
tts_error_text_unknown
tts_error_text_timeout
tts_error_text_transport
11. 第一版工程落地建议

如果你下一步要把这套文档变成 C++ 代码，建议最先落地下面这几个枚举和接口。

11.1 状态枚举
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
  kShuttingDown,
};
11.2 事件枚举
enum class VisEventType {
  kBootCompleted,
  kBootFailed,
  kWakeSpeechDetected,
  kKwsMatched,
  kKwsRejected,
  kKwsTimeout,
  kTtsPlaybackCompleted,
  kTtsPlaybackFailed,
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
  kFatalError,
  kRecoverySucceeded,
  kRecoveryFailed,
  kShutdownRequested,
};
11.3 事件结构
struct VisEvent {
  VisEventType type;
  std::string text;
  std::string request_id;
  // 可扩展：intent / slots / error_code / payload
};

# MOS-VIS 架构设计（Architecture Spec）

来源：`SystemDesgin.md` 专题拆分  
范围：系统目标、分层架构、状态机、接口与性能基线

## 1. 目标与约束
- 目标：低延迟、可打断、可降级的嵌入式语音交互。
- 音频标准：`S16_LE` / `16000Hz` / `mono`。
- 资源目标：内存 < 200 MB，控制链路 P95 < 1.5 s。

## 2. 总体架构

### 2.1 分层
```text
应用服务层：SessionManager / PipelineScheduler / ContextManager
算法引擎层：AudioPreprocessor / VAD / KWS / ASR / NLU / RAG / TTS
硬件抽象层：ALSA/PortAudio / RingBuffer / RKNN / DSP
```

### 2.2 组件依赖
```text
VoiceInteractiveAgent
  -> SessionManager / PipelineScheduler / ContextManager
  -> AudioCapture/Playback/RingBuffer
  -> Vad/Kws/Asr/Nlu/Rag/Tts
  -> ControlClient(WebSocket)
```

### 2.3 线程模型
- Main Thread：生命周期、配置、状态调度。
- Audio Capture Thread：PortAudio 回调写环形缓冲。
- Engine Worker Threads：VAD/KWS/ASR/NLU/RAG/TTS。
- WebSocket I/O Thread：收发、重连、发送队列。

并发策略：
- RingBuffer：无锁单写多读。
- 会话状态/上下文：互斥保护。
- I/O 队列：线程安全队列。

## 3. Pipeline 状态机
统一状态：`IDLE -> LISTENING -> RECOGNIZING -> EXECUTING -> SPEAKING -> IDLE`

- WakeupPipeline：`IDLE -> VAD1 -> KWS -> ACK(TTS) -> LISTENING`
- RecognitionPipeline：`LISTENING -> VAD2 -> ASR -> RECOGNIZING`
- ControlPipeline：`RECOGNIZING -> NLU(control) -> WS -> EXECUTING -> SPEAKING`
- KnowledgePipeline：`RECOGNIZING -> NLU(knowledge) -> RAG -> SPEAKING`

打断策略：`interrupt_enabled=true` 时，`SPEAKING` 中检测到唤醒词立即中断并进入 `LISTENING`。

## 4. 硬软协同

### 4.1 任务划分
- NPU：VAD/KWS/ASR。
- CPU：NLU、RAG HTTP、TTS。
- DSP/CPU：NS/AEC（优先 DSP）。

### 4.2 内存预算
- RingBuffer：320 KB（10s）。
- 模型：VAD 2MB + KWS 5MB + ASR 20MB + TTS 30MB。
- 其他运行时：约 50~130MB。
- 总目标：< 200MB。

### 4.3 采集与缓存
- 推荐 ALSA `SND_PCM_ACCESS_MMAP_INTERLEAVED`。
- 20ms 固定块（320 samples）作为跨模块粒度。

## 5. 算法与模型策略
- 双 VAD：前门控 + 语音段边界。
- AudioPreprocessor：`webrtc_ns` + `webrtc_aec`。
- NLU：`re2` + DFA + 热重载。
- VAD 阈值：支持底噪自适应（最近 100 帧）。

## 6. 接口协议

### 6.1 WebSocket
- 请求字段：`id/action/params`
- 响应字段：`id/status/code/message`
- 错误码：`200/400/401/404/500/503`

认证首包：
```json
{"type":"auth","token":"<pre-shared-token>"}
```

### 6.2 RAG
- `rag.endpoint` 支持多 URL fallback。
- 默认超时 5000ms，默认重试 1 次。

### 6.3 本地控制
- `--local-socket /var/run/mos-vis.sock`

## 7. 性能基线
- 唤醒延迟 < 300ms。
- 控制链路 P95 < 1.5s。
- 引擎耗时：VAD/KWS/ASR 单次目标 < 10ms。

必测输出：
- 平均耗时、P95/P99、CPU/NPU 占用、峰值内存、吞吐。

## 8. 相关文档
- 总规格：[SystemDesgin.md](../SystemDesgin.md)
- 可靠性专题：[reliability.md](./reliability.md)
- 安全专题：[security.md](./security.md)

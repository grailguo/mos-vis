# MOS-VIS 语音交互系统设计规格书（目标态）

版本：v1.0-draft  
状态：Design Baseline  
适用平台：RK3588 + Ubuntu 20.04 aarch64

## 文档导航
- 总规格（本文件）：`SystemDesign.md`
- 架构专题：`docs/architecture.md`
- 可靠性专题：`docs/reliability.md`
- 安全专题：`docs/security.md`

---

## 1. 背景与目标

### 1.1 背景
MOS-VIS 面向嵌入式语音交互场景，提供从唤醒、识别、理解、执行到播报的完整链路。系统运行于 RK3588 平台，采用 NPU 加速关键语音推理任务。

### 1.2 业务目标
- 提供低延迟、可打断的语音交互体验。
- 支持设备控制、知识查询、基础对话三类主流程。
- 在网络不稳定或部分组件异常时维持可降级运行。

### 1.3 非目标
- 本版不覆盖完整云端训练平台。
- 本版不覆盖多语言大规模模型管理平台。
- 本版不承诺一次性完成全部法规认证流程（进入路线图）。

### 1.4 设计约束
- 峰值内存预算：< 200 MB（核心服务 + 模型驻留 + 缓冲）。
- 端到端延迟目标：控制类请求 P95 < 1.5 s。
- 音频标准：PCM `S16_LE` / `16000Hz` / `mono`。

---

## 2. 总体架构

### 2.1 分层架构

```text
┌───────────────────────────────────────────────────────────┐
│ 应用服务层                                                │
│ VoiceInteractiveAgent / SessionManager / PipelineScheduler│
│ ContextManager / Control API / Diagnostics               │
└────────────────────────────┬──────────────────────────────┘
                             │
┌────────────────────────────▼──────────────────────────────┐
│ 算法引擎层                                                 │
│ AudioPreprocessor(NS/AEC) / VAD1 / KWS / VAD2 / ASR      │
│ NLU / RAG Client / TTS                                    │
└────────────────────────────┬──────────────────────────────┘
                             │
┌────────────────────────────▼──────────────────────────────┐
│ 硬件抽象层                                                 │
│ ALSA/PortAudio / AudioRingBuffer / RKNN Runtime / DSP    │
└───────────────────────────────────────────────────────────┘
```

### 2.2 组件依赖关系

```text
VoiceInteractiveAgent
  ├─ SessionManager
  ├─ PipelineScheduler
  ├─ ContextManager
  ├─ AudioCapture / AudioPlayback / AudioRingBuffer
  ├─ VadEngine(dual) + KwsEngine + AsrEngine
  ├─ NluEngine (Simple/Classifier/LLM)
  ├─ ControlClient(WebSocket)
  ├─ RagClient(HTTP)
  └─ TtsEngine
```

### 2.3 线程拓扑与并发模型

```text
Main Thread
  ├─ 生命周期管理、配置加载、信号处理
  ├─ SessionManager 主状态调度
  └─ PipelineScheduler 调度

Audio Capture Thread (PortAudio callback)
  └─ 采集音频并写入 AudioRingBuffer

Engine Worker Threads (可配置)
  ├─ VAD/KWS/ASR 工作线程
  ├─ NLU/RAG/TTS 工作线程
  └─ 任务队列消费

WebSocket I/O Thread
  ├─ 控制消息收发
  └─ 重连与待发送队列处理
```

并发约束：
- `AudioRingBuffer` 为无锁单写多读模型。
- 会话状态与上下文写操作通过互斥保护。
- WebSocket 发送队列使用线程安全队列。

---

## 3. Pipeline 状态机

### 3.1 统一会话状态
`IDLE -> LISTENING -> RECOGNIZING -> EXECUTING -> SPEAKING -> IDLE`

### 3.2 WakeupPipeline

```text
IDLE -> VAD1 Gate Open -> KWS Hit -> Wake ACK(TTS) -> LISTENING
```

### 3.3 RecognitionPipeline

```text
LISTENING -> VAD2 Speech Start -> ASR Streaming -> VAD2 Speech End -> RECOGNIZING
```

### 3.4 ControlPipeline

```text
RECOGNIZING -> NLU(DeviceControl) -> WS Request -> EXECUTING -> SPEAKING -> IDLE
```

### 3.5 KnowledgePipeline

```text
RECOGNIZING -> NLU(KnowledgeQuery) -> RAG HTTP -> SPEAKING -> IDLE
```

### 3.6 打断策略
- 当 `interrupt_enabled=true` 且系统处于 `SPEAKING`，若检测到新唤醒词：
  - 立即停止 TTS。
  - 清理当前播报任务。
  - 状态切换到 `LISTENING`。

---

## 4. 硬软协同设计

### 4.1 CPU/NPU/DSP 任务划分

| 子系统 | 执行单元 | 说明 |
|---|---|---|
| VAD/KWS/ASR 推理 | NPU | RKNN INT8 推理 |
| NLU 规则与分类 | CPU | 规则匹配 + 分类模型 |
| RAG HTTP | CPU | 网络请求与响应解析 |
| TTS 推理与播放 | CPU（后续可扩展 NPU/DSP） | 本地播报 |
| NS/AEC | DSP 或 CPU | 优先 DSP，CPU 作为回退 |

典型负载（目标）：
- NPU 占用率：30%~55%（持续对话场景）。
- CPU 占用率：20%~45%（含网络与TTS）。

### 4.2 内存资源预算

| 模块 | 预算 |
|---|---|
| AudioRingBuffer（10s×16k×2B） | 320 KB |
| VAD 模型 | 2 MB |
| KWS 模型 | 5 MB |
| ASR 模型 | 20 MB |
| TTS 模型 | 30 MB |
| 队列、上下文、控制缓存 | 20~60 MB |
| 运行时开销（日志/线程栈/碎片） | 30~70 MB |
| 合计目标 | < 200 MB |

### 4.3 DMA 与缓存策略
- 音频采集优先使用 DMA 双缓冲。
- ALSA 推荐访问模式：`SND_PCM_ACCESS_MMAP_INTERLEAVED`。
- 20ms 固定帧（320 samples）作为跨模块基础处理粒度。

### 4.4 功耗策略
- 待机模式：仅轻量 VAD 运行，NPU 可休眠。
- 活跃模式：唤醒后提升 CPU 频率与 NPU 工作频率。
- 配置项：`power.idle_poll_interval_ms`、`power.dvfs_enabled`。

---

## 5. 算法设计

### 5.1 音频预处理
新增 `AudioPreprocessor`：
- 降噪：`webrtc_ns`。
- 回声消除：`webrtc_aec`。
- 配置：

```json
{
  "audio": {
    "preprocessing": {
      "ns_enabled": true,
      "aec_enabled": true
    }
  }
}
```

### 5.2 双 VAD
- `VAD1`：KWS 前门控，降低误触发。
- `VAD2`：语音段边界检测，驱动 ASR 截断。
- 增加动态阈值：按最近 100 帧底噪能量自适应。

### 5.3 KWS 指标
- 安静环境唤醒率：>= 98%。
- 噪声环境（SNR=10dB）唤醒率：>= 93%。
- 误唤醒率：<= 0.2 次/小时。

### 5.4 ASR 指标
- 中文测试集 WER：< 10%。
- 支持热词与自定义词表。

### 5.5 NLU 引擎策略
- 规则引擎采用 `re2`，避免灾难性回溯。
- 规则预编译为 DFA。
- 支持规则热重载。

---

## 6. 接口与通信协议

### 6.1 WebSocket 控制协议
请求：

```json
{
  "id": "req-001",
  "action": "light.set",
  "params": {
    "zone": "living_room",
    "brightness": 80
  }
}
```

响应：

```json
{
  "id": "req-001",
  "status": "ok",
  "code": 200,
  "message": "success"
}
```

### 6.2 认证机制
- 首包认证：

```json
{
  "type": "auth",
  "token": "<pre-shared-token>"
}
```

- 配置项：
  - `control.auth_token`
  - `control.wss_enabled`
  - `control.tls_cert_path`
  - `control.tls_key_path`

### 6.3 错误码标准

| code | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 请求格式错误 |
| 401 | 认证失败 |
| 404 | 动作不存在 |
| 500 | 内部错误 |
| 503 | 设备繁忙 |

### 6.4 RAG 接口
- 配置：`rag.endpoint` 支持数组与 fallback。

```json
{
  "rag": {
    "endpoint": [
      "https://rag-a.local/rag",
      "https://rag-b.local/rag"
    ],
    "timeout_ms": 5000,
    "retry": 1
  }
}
```

### 6.5 本地控制接口
- 参数：`--local-socket /var/run/mos-vis.sock`
- 用途：本地进程控制（禁用唤醒、查询状态、安全敏感操作）。

### 6.6 音频格式标准
- 输入流：`S16_LE`, `16000Hz`, `mono`
- 数据块：`320 samples`（20ms）

---

## 7. 性能指标与基准

### 7.1 核心指标
- 唤醒延迟：< 300 ms
- 控制端到端 P95：< 1.5 s
- 单次推理耗时（目标）：VAD/KWS/ASR 各 < 10 ms

### 7.2 基准测试输出
基准工具必须输出：
- 各引擎平均耗时、P95/P99。
- CPU/NPU 占用率。
- 内存峰值。
- 吞吐（每分钟可处理会话数）。

### 7.3 环形缓冲策略
- `ring_seconds` 支持配置化。
- 输出溢出统计与告警。
- 频繁溢出触发告警事件并建议扩容。

### 7.4 剖析工具
- 内置 `LatencyProbe`，记录阶段级时间戳。
- 支持日志输出与 Prometheus 导出。

---

## 8. 可靠性与容错

### 8.1 设备故障处理
- ALSA 异常时每 1s 重试，最多 10 次。
- 失败后切换备用设备（`default`）并告警。

### 8.2 模型加载失败降级
- 单引擎初始化失败时禁用该引擎，不中止主服务。
- 记录错误并上报告警。

### 8.3 WebSocket 重连
- 指数退避：1s,2s,4s,8s,16s... 最大 60s。
- 待发送缓存最多 10 条控制指令。

### 8.4 看门狗
- 软件看门狗线程每 1s 检查主循环心跳。
- 超过 5s 无响应触发进程重启流程。

### 8.5 systemd 恢复策略

```ini
Restart=always
RestartSec=5
StartLimitBurst=3
StartLimitIntervalSec=60
```

### 8.6 降级运行模式
- `FULL`：所有能力开启。
- `RECOGNITION_ONLY`：禁用 KWS，改按键唤醒。
- `OFFLINE`：禁用 RAG，仅本地 NLU。

---

## 9. 安全与隐私

### 9.1 传输安全
- WebSocket 支持 WSS（TLS）。
- 明文 WS 仅限开发环境。

### 9.2 访问控制
- 敏感操作（如恢复出厂设置）仅允许：
  - 本地 Unix Socket，或
  - 已认证且授权级别满足的远程调用。

### 9.3 数据生命周期
- 会话结束后可清零环形缓冲区音频数据。
- ASR 文本默认不持久化。
- 若启用调试上传，需显式用户同意。

### 9.4 本地数据保护
- 用户偏好/调试日志可选 AES-256 加密存储。
- 密钥建议由受保护分区或硬件安全模块管理。

### 9.5 合规声明
- 默认不上传语音原始数据。
- 支持日志清除 API。
- 对应法规要求见第 13 章。

---

## 10. 用户体验与交互设计

### 10.1 多轮对话上下文
- `ContextHistory` 保存最近 2~3 轮意图与槽位。
- 支持指代消解（如“把它调亮”）。

### 10.2 打断与恢复
- 播报期间支持唤醒打断。
- 打断后优先进入聆听态并播报简短提示音。

### 10.3 超时反馈
- 8s 无语音时先提示“请问还有什么需要吗？”。
- 再等待 3s，仍无输入则结束会话。

### 10.4 可视反馈
- 通过 D-Bus 广播状态：`listening/processing/speaking/idle`。
- 桌面插件显示图标或文本状态。

### 10.5 用户偏好
- `user_preferences.json` 支持：
  - 唤醒灵敏度
  - TTS 语速
  - 音量
- 支持运行时热更新。

---

## 11. 测试与验证

### 11.1 单元测试
- 框架：GoogleTest。
- 引擎接口提供 Mock。
- 目标覆盖率：> 80%。

### 11.2 集成测试
- 用例：音频输入 -> JSON 行为输出。
- CI：GitHub Actions（QEMU aarch64 或真机 Runner）。

### 11.3 性能测试
- 采集引擎耗时、端到端延迟、吞吐、资源占用。
- 输出环境信息与测试样本版本，保证可复现。

### 11.4 压力与稳定性
- 连续运行 72h。
- 故障注入：网络中断、设备插拔、模型缺失。
- 验收：可恢复且无致命崩溃。

### 11.5 场景测试
- 场景：安静、噪声、多人、回声房间。
- 指标：唤醒率、误唤醒率、WER、响应时延。

### 11.6 兼容性测试
- 不同 RK3588 板卡。
- 不同 ALSA 设备。
- 不同 Ubuntu 补丁版本。

---

## 12. 运维与升级

### 12.1 诊断能力
- 命令：`--diagnose`
- 自检项：音频设备、模型加载、NPU、WebSocket 连通性。

### 12.2 日志与可观测
- 统一结构化日志（建议 JSON）。
- 关键事件：状态迁移、超时、重连、降级。

### 12.3 OTA 升级
- 升级包内容：二进制、配置、模型。
- 全流程：下载 -> 签名校验 -> 原子替换 -> 重启。

### 12.4 回滚策略
- 升级失败自动回滚至上一稳定版本。
- 回滚结果写入事件日志并上报。

---

## 13. 合规与供应链

### 13.1 标准与认证
- 音频接口遵循 ALSA API 标准。
- 无线能力产品需通过 FCC/CE/RED（按上市地区）。

### 13.2 开源许可证合规
- 维护第三方依赖许可证清单。
- 提供 `NOTICE` 文件。

### 13.3 隐私法规
- 中国市场：遵循《个人信息保护法》。
- 海外市场：遵循 GDPR/CCPA（按地区启用策略）。

### 13.4 BOM 与成本

| 组件 | 参考成本 |
|---|---|
| RK3588 | ~30 USD |
| RAM/Flash | 8~15 USD |
| 麦克风阵列 | 5~15 USD |
| 音频 Codec | ~2 USD |
| 其他器件 | 15~30 USD |
| 总计目标 | < 100 USD |

### 13.5 供应链与替代
- 备选 SoC：RK3568、全志 V853。
- 麦克风提供一级/二级替代型号。
- 关键器件维护长期供货与 EOL 跟踪。

---

## 14. 里程碑与优先级（P0/P1/P2）

### 14.1 P0（阻塞开发，必须完成）
- 可靠性：异常处理、降级模式、看门狗、热插拔恢复。
- 安全：WebSocket 认证、WSS 传输、隐私声明。
- 测试：单元测试框架、集成测试、场景验证计划。

### 14.2 P1（显著影响质量）
- 多轮对话上下文。
- 降噪 + AEC。
- 性能实测基准数据。
- OTA 与回滚流程。

### 14.3 P2（优化项）
- 线程模型图与状态机文档精化。
- 功耗优化策略完善。
- 本地 Unix Socket 控制通道。
- 合规声明与供应链扩展。

---

## 附录 A：评审意见处理矩阵

| 编号 | 评审主题 | 状态 | 落点章节 | 验收标准 |
|---|---|---|---|---|
| A1 | 线程模型与并发说明 | Accepted-Now | 2.3/3 | 线程图 + 互斥/无锁说明完整 |
| A2 | SessionController 拆分 | Accepted-Roadmap | 2/14 | 模块职责拆分并列入里程碑 |
| A3 | 状态机定义 | Accepted-Now | 3 | Pipeline 状态图可追踪 |
| A4 | 组件依赖图 | Accepted-Now | 2.2 | 依赖关系明确可读 |
| B1 | CPU/NPU 划分 | Accepted-Now | 4.1 | 任务与执行单元映射完整 |
| B2 | 内存分配表 | Accepted-Now | 4.2 | 模块级预算 + 总量目标 |
| B3 | DMA/缓存策略 | Accepted-Now | 4.3 | 给出 ALSA 推荐模式 |
| B4 | 功耗估算 | Accepted-Roadmap | 4.4/7 | 场景功耗基准待实测 |
| C1 | AEC/NS | Accepted-Now | 5.1 | 配置项 + 模块设计存在 |
| C2 | KWS 准确率指标 | Accepted-Now | 5.3 | 指标定义含 SNR=10dB |
| C3 | ASR WER 指标 | Accepted-Now | 5.4 | WER 与测试集口径明确 |
| C4 | NLU 性能优化 | Accepted-Now | 5.5 | re2 + DFA + 热重载 |
| C5 | VAD 自适应阈值 | Accepted-Now | 5.2 | 自适应策略定义 |
| D1 | WS 认证 | Accepted-Now | 6.2 | 首包认证 + 配置定义 |
| D2 | 错误码标准 | Accepted-Now | 6.3 | 错误码表完整 |
| D3 | RAG 可配置与 fallback | Accepted-Now | 6.4 | endpoint 数组示例 |
| D4 | 本地 Socket | Accepted-Roadmap | 6.5/14 | 接口定义 + 里程碑 |
| D5 | 音频格式标准化 | Accepted-Now | 6.6 | 采样规格固定 |
| E1 | 实测基准数据 | Deferred | 7 | 数据待性能测试补齐 |
| E2 | 待机节能策略 | Accepted-Now | 4.4/7 | idle 策略与配置存在 |
| E3 | RingBuffer 可调/告警 | Accepted-Now | 7.3 | 溢出统计机制定义 |
| E4 | DSP 利用 | Accepted-Roadmap | 4.1/14 | 扩展方向明确 |
| E5 | 性能剖析工具 | Accepted-Now | 7.4 | LatencyProbe 设计定义 |
| F1 | 可靠性容错全套 | Accepted-Now | 8 | 重试/重连/降级明确 |
| G1 | 多轮对话 | Accepted-Roadmap | 10.1/14 | 上下文管理列入 P1 |
| G2 | 打断策略 | Accepted-Now | 3.6/10.2 | 打断行为定义 |
| G3 | 可视反馈 | Accepted-Roadmap | 10.4/14 | DBus 状态接口定义 |
| G4 | 超时反馈优化 | Accepted-Now | 10.3 | 二段超时策略定义 |
| G5 | 用户偏好 | Accepted-Roadmap | 10.5/14 | 偏好配置与热更新规划 |
| H1 | 安全与隐私体系 | Accepted-Now | 9 | 传输/存储/权限全覆盖 |
| I1 | 测试体系 | Accepted-Now | 11 | 单测/集成/场景/压力齐全 |
| J1 | 可维护性与文档化 | Accepted-Now | 12/14 | 诊断、OTA、版本策略 |
| K1 | 合规标准 | Accepted-Roadmap | 13/14 | 按市场阶段推进认证 |
| L1 | 成本与供应链 | Accepted-Roadmap | 13.4/13.5 | BOM 与替代方案定义 |

---

## 附录 B：术语与命名规范

- Pipeline：处理流水线。  
- Session：单次语音交互会话。  
- Intent：NLU 意图分类结果。  
- Slot：意图参数槽位。  
- 配置项命名：小写 + 下划线。  
- 指标单位统一：`ms`、`MB`、`mA`、`%`。

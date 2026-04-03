# Voice Interactive System for RK3588 + NPU

## 1. 概述

本文档描述一个面向嵌入式平台（RK3588 + NPU）的语音交互系统。系统采用客户端-服务端架构，提供从语音唤醒、语音识别、自然语言理解到设备控制与知识检索的完整交互能力。

- 运行环境：Ubuntu 20.04 aarch64 + Xfce Desktop
- 硬件加速：RK3588 内置 NPU（6 TOPS INT8）

系统由两个核心组件构成：

- `mos-vis-library`：C++ 核心库，封装音频采集/播放、VAD、KWS、ASR、NLU、RAG、TTS、WebSocket 控制客户端
- `mos-vis-server`：基于核心库的 `systemd` 服务，负责配置加载、日志初始化、服务生命周期管理与信号处理

## 2. 系统架构

整体分为三层：硬件抽象层、算法引擎层、应用服务层。

```text
┌─────────────────────────────────────────────────────────────┐
│ mos-vis-server (Service)                                   │
│ - CLI / Config (JSON) - spdlog - Signal Handler            │
│ - VoiceInteractiveAgent - SessionController                 │
└───────────────────────────┬─────────────────────────────────┘
                            │ depends on
┌───────────────────────────▼─────────────────────────────────┐
│ mos-vis-library (Core Library)                              │
│ ┌──────────────┬──────────────┬──────────────┬───────────┐  │
│ │ AudioCapture │ AudioPlayback│ AudioRingBuf │ DeviceSel │  │
│ └──────────────┴──────────────┴──────────────┴───────────┘  │
│ ┌──────────────┬──────────────┬──────────────┬───────────┐  │
│ │ RknnVadEngine│SherpaKwsEngine│SherpaAsrEngine│SimpleNLU│  │
│ │ClassifierNluEngine│LanguageModelNluEngine               │  │
│ └──────────────┴──────────────┴──────────────┴───────────┘  │
│ ┌──────────────┬──────────────┬──────────────────────────┐  │
│ │ StubRAGEngine│SherpaTtsEngine│ ControlClient(WebSocket)│  │
│ └──────────────┴──────────────┴──────────────────────────┘  │
│ ┌─────────────────────────────────────────────────────────┐  │
│ │ SessionController & Pipelines                          │  │
│ │ Wakeup | Recognition | Control | Knowledge | Speak     │  │
│ └─────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

**数据流**：音频输入 -> 环形缓冲区 -> VAD 门控 -> 唤醒词/语音识别 -> NLU -> 控制指令/知识查询 -> TTS 播报 -> 音频输出。

## 3. 硬件平台与依赖

### 3.1 目标平台

- SoC：Rockchip RK3588（4x Cortex-A76 + 4x Cortex-A55）
- NPU：6 TOPS INT8，支持 RKNN 推理框架
- OS：Ubuntu 20.04.6 LTS aarch64
- 桌面环境：Xfce 4.14
- 音频硬件：ALSA 兼容麦克风阵列与扬声器

### 3.2 软件依赖

| 依赖 | 版本/来源 | 用途 |
|---|---|---|
| `spdlog` | >= 1.8 | 异步日志 |
| `nlohmann_json` | >= 3.9 | 配置与消息序列化 |
| `Boost.Beast` | Boost 1.71+ | WebSocket 异步客户端 |
| `PortAudio` | >= 19.6 | 跨平台音频采集/播放 |
| `RKNN Runtime` | `rknn_api.h` / `librknnrt.so` | NPU 推理（VAD/KWS/ASR） |
| `sherpa-onnx C++ API` | 自编译或预置库 `libsherpa-onnx-cxx-api.so` | 语音算法引擎 |

> 注意：`sherpa-onnx` 需支持 `aarch64 + RKNN` 后端，建议放在 `${PROJECT_SOURCE_DIR}/3rdparty/`。

## 4. 软件组件详细设计

### 4.1 `mos-vis-library`

#### 4.1.1 模块职责

| 模块 | 职责 |
|---|---|
| `AudioCapture` | 采集 PCM 音频并推入环形缓冲区 |
| `AudioPlayback` | 播放 TTS 音频或提示音 |
| `AudioRingBuffer` | 无锁环形缓冲，支持多消费者 |
| `AudioDeviceSelector` | 枚举 ALSA 设备并选择输入输出 |
| `RknnVadEngine` | RKNN VAD 门控 |
| `SherpaKwsEngine` | 唤醒词检测 |
| `SherpaAsrEngine` | 语音识别 |
| `SimpleNluEngine` | 规则 NLU（正则/关键词） |
| `ClassifierNluEngine` | 规则 fast-path + 轻量分类器 fallback |
| `LanguageModelNluEngine` | 规则 + 小模型 + LLM fallback |
| `StubRAGEngine` | HTTP 调用远程 RAG 服务 |
| `SherpaOfflineTtsEngine` | 离线 TTS |
| `ControlClient` | WebSocket 指令发送与响应接收 |
| `SessionController` | 会话状态管理与流水线调度 |

#### 4.1.2 核心接口（`VoiceInteractiveAgent`）

```cpp
namespace mos::vis {

class VoiceInteractiveAgent {
public:
    explicit VoiceInteractiveAgent(const nlohmann::json& config);
    ~VoiceInteractiveAgent();

    bool Initialize();
    bool Start();
    void Stop();
    bool IsRunning() const;

private:
    std::unique_ptr<SessionController> session_ctrl_;
    std::unique_ptr<AudioCapture> capture_;
    // ... 其他引擎组件
};

} // namespace mos::vis
```

### 4.2 `mos-vis-server`

#### 4.2.1 命令行参数

| 参数 | 说明 | 默认值 |
|---|---|---|
| `-c`, `--config` | 配置文件路径 | `/etc/mos/vis/config/config.json`（release） / `./config/config.json`（develop） |
| `-l`, `--log-level` | 日志级别：`trace/debug/info/warn/error` | `info` |
| `-d`, `--daemon` | 守护进程模式（`systemd` 下不建议） | `false` |

#### 4.2.2 生命周期管理

1. 解析命令行参数并确定配置路径。
2. 初始化 `spdlog`（控制台 + `syslog`）。
3. 加载并校验 JSON 配置。
4. 实例化 `VoiceInteractiveAgent`，调用 `Initialize()` 与 `Start()`。
5. 注册 `SIGINT` / `SIGTERM` 处理器。
6. 等待退出信号，调用 `Stop()` 并释放资源。

#### 4.2.3 开发环境支持

- 使用 VSCode + CMakeTools，默认构建类型 `Debug`
- 开发运行默认使用 `./config/config.json`

## 5. 核心引擎与算法

### 5.1 VAD（Voice Activity Detection）

系统采用双 VAD：

- `VAD1`：位于唤醒流水线，作为 KWS 前置门控
- `VAD2`：位于识别流水线，精确检测语音边界

实现类：`RknnVadEngine`（Silero VAD v4 RKNN）

### 5.2 KWS（Keyword Spotting）

- 实现：`SherpaKwsEngine`
- 模型：Zipformer 中英文唤醒词模型（3M 参数）
- 行为：唤醒后触发 `WakeupPipeline`，根据 `wake_ack_text` 匹配回复

### 5.3 ASR（Automatic Speech Recognition）

- 实现：`SherpaAsrEngine`
- 模型：流式 Zipformer 双语模型（中文+英文，RKNN 推理）
- 输出：识别文本，送入 NLU

### 5.4 NLU（Natural Language Understanding）

系统支持三种 NLU，通过配置选择：

| 引擎 | 适用场景 | 特点 |
|---|---|---|
| `SimpleNluEngine` | 固定命令集 | 规则匹配 + 槽位正则 |
| `ClassifierNluEngine` | 意图较多但命令模式稳定 | 规则 fast-path + 轻量分类器 |
| `LanguageModelNluEngine` | 复杂语义/对话 | 规则 -> 小模型 -> LLM fallback + 校验器 |

输出结构（`IntentResult`）：

```cpp
enum class IntentType { kUnknown, kWakeup, kDeviceControl, kKnowledgeQuery, kChat };

struct IntentResult {
    IntentType intent;
    std::string text;     // 原始识别文本
    std::string action;   // 标准化动作，如 "light.on"
    nlohmann::json slots; // 槽位，如 {"brightness": 80}
};
```

### 5.5 RAG（Retrieval-Augmented Generation）

- 实现：`StubRAGEngine`
- 接口：`POST http://172.16.1.100:8081/rag`
- 请求：`{"query": "<用户问题>"}`
- 响应：`{"answer": "<检索增强后的答案>"}`
- 超时策略：连接超时 5s，最多重试 1 次

### 5.6 TTS（Text to Speech）

- 实现：`SherpaOfflineTtsEngine`
- 模型：VITS Melo TTS（中英文）
- 输出：PCM 音频 -> `AudioPlayback`

## 6. 交互流程

系统定义四条主要 Pipeline，由 `SessionController` 调度。

### 6.1 唤醒流程（WakeupPipeline）

```text
麦克风 -> AudioRingBuffer -> VAD1(门控) -> SherpaKwsEngine
                                             |
                                             v 唤醒事件
                              匹配 wake_ack_text 中关键词
                                             |
                                             v
                              TTS 播放对应的 reply_text
```

### 6.2 设备控制流程（Recognition -> Control）

```text
唤醒后语音输入 -> VAD2 -> ASR -> NLU
                             |
                       IntentType::kDeviceControl
                             |
                             v
                   ControlClient (WebSocket)
                             |
                             v 异步 JSON 请求
                    等待设备响应（超时 3 秒）
                             |
                             v
                     TTS 播报执行结果
```

### 6.3 知识查询流程（KnowledgePipeline）

```text
语音输入 -> VAD2 -> ASR -> NLU
                       |
                 IntentType::kKnowledgeQuery
                       |
                       v
             StubRAGEngine -> HTTP 请求
                       |
                       v
               TTS 播报 RAG 答案
```

### 6.4 会话与超时

- 每次唤醒后开启会话，8 秒无有效指令自动结束
- 控制指令异步等待响应，超时提示“设备无响应”

## 7. 配置与部署

### 7.1 配置文件结构

- 生产环境：`/etc/mos/vis/config/config.json`
- 开发环境：`./config/config.json`

生产环境示例：

```json
{
  "audio": {
    "sample_rate": 16000,
    "channels": 1,
    "capture_chunk_samples": 320,
    "ring_seconds": 10,
    "input_device": "AIUI-USB-MC",
    "output_device": "rockchip-es8388",
    "channel_select_mode": "fixed",
    "fixed_channel_index": 5,
    "track_switch_consecutive": 3
  },
  "vad1": {
    "enabled": true,
    "model_path": "/usr/share/mos/vis/models/vad/silero-vad-v4-rk3588.rknn",
    "threshold": 0.35,
    "window_samples": 512,
    "hop_samples": 160,
    "open_frames": 2,
    "close_frames": 10,
    "hangover_ms": 160
  },
  "kws": {
    "enabled": true,
    "model_dir": "/usr/share/mos/vis/models/kws/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20",
    "chunk_samples": 320,
    "preroll_ms": 400
  },
  "vad2": {
    "enabled": true,
    "model_path": "/usr/share/mos/vis/models/vad/silero-vad-v4-rk3588.rknn",
    "threshold": 0.5,
    "window_samples": 512,
    "hop_samples": 160,
    "start_frames": 3,
    "end_frames": 20,
    "hangover_ms": 320
  },
  "asr": {
    "enabled": true,
    "model_dir": "/usr/share/mos/vis/models/asr/sherpa-onnx-rk3588-streaming-zipformer-bilingual-zh-en-2023-02-20",
    "chunk_samples": 320,
    "preroll_ms": 400,
    "tail_ms": 240
  },
  "tts": {
    "enabled": true,
    "model_dir": "/usr/share/mos/vis/models/tts/vits-melo-tts-zh_en",
    "use_int8": true,
    "fixed_phrase_cache": true
  },
  "wake_ack_text": [
    {
      "keywords": ["0801"],
      "preset_file": "",
      "reply_text": "收到，请说。"
    },
    {
      "keywords": ["0803"],
      "preset_file": "",
      "reply_text": "我在，请指示。"
    },
    {
      "keywords": ["小G"],
      "preset_file": "",
      "reply_text": "在呢，请继续。"
    },
    {
      "keywords": ["小莫"],
      "preset_file": "",
      "reply_text": "我在，请讲。"
    }
  ]
}
```

### 7.2 模型目录结构（生产环境）

所有模型统一安装于 `/usr/share/mos/vis/models/`：

```text
/usr/share/mos/vis/models/
├── vad/
│   └── silero-vad-v4-rk3588.rknn
├── kws/
│   └── sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20/
│       ├── model.onnx
│       ├── tokens.txt
│       └── ...
├── asr/
│   └── sherpa-onnx-rk3588-streaming-zipformer-bilingual-zh-en-2023-02-20/
│       ├── encoder.onnx
│       ├── decoder.onnx
│       ├── joiner.onnx
│       └── tokens.txt
└── tts/
    └── vits-melo-tts-zh_en/
        ├── model.onnx
        └── lexicon.txt
```

### 7.3 开发环境配置

开发时建议在项目根目录维护 `./config/config.json`，并使用相对路径模型目录。

```json
{
  "vad1": {
    "model_path": "./models/vad/silero-vad-v4-rk3588.rknn"
  },
  "kws": {
    "model_dir": "./models/kws/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20"
  }
}
```

本地 `./models/` 可使用软链接或独立拷贝。

### 7.4 配置参数速查

| 节 | 参数 | 类型 | 说明 |
|---|---|---|---|
| `audio` | `sample_rate` | int | 16000 Hz |
| `audio` | `channels` | int | 输入通道数 |
| `audio` | `capture_chunk_samples` | int | 320 = 20ms |
| `audio` | `ring_seconds` | int | 环形缓冲时长 |
| `audio` | `input_device` | string | ALSA 输入设备名 |
| `audio` | `output_device` | string | ALSA 输出设备名 |
| `audio` | `channel_select_mode` | string | `fixed` 或 `dynamic` |
| `audio` | `fixed_channel_index` | int | 固定通道索引（0-based） |
| `audio` | `track_switch_consecutive` | int | 动态切换连续检测次数 |
| `vad1/vad2` | `enabled` | bool | 启用开关 |
| `vad1/vad2` | `model_path` | string | RKNN 模型路径 |
| `vad1/vad2` | `threshold` | float | 语音阈值（0-1） |
| `kws` | `model_dir` | string | KWS 模型目录 |
| `asr` | `model_dir` | string | ASR 模型目录 |
| `tts` | `model_dir` | string | TTS 模型目录 |
| `wake_ack_text` | `keywords/reply_text` | array/string | 唤醒词与回复映射 |

### 7.5 安装脚本

```bash
# 创建配置目录并复制配置
sudo mkdir -p /etc/mos/vis/config
sudo cp config/config.json /etc/mos/vis/config/

# 创建模型目录并复制模型
sudo mkdir -p /usr/share/mos/vis/models
sudo cp -r models/vad /usr/share/mos/vis/models/
sudo cp -r models/kws /usr/share/mos/vis/models/
sudo cp -r models/asr /usr/share/mos/vis/models/
sudo cp -r models/tts /usr/share/mos/vis/models/

# 设置权限
sudo chmod -R 755 /usr/share/mos/vis/models
```

### 7.6 `systemd` 服务单元

`/etc/systemd/system/mos-vis.service`

```ini
[Unit]
Description=MOS Voice Interactive Service
After=network.target sound.target
Wants=sound.target

[Service]
Type=simple
User=root
ExecStart=/usr/local/bin/mos_vis_server -c /etc/mos/vis/config/config.json
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal
SyslogIdentifier=mos-vis

[Install]
WantedBy=multi-user.target
```

部署命令：

```bash
sudo systemctl daemon-reload
sudo systemctl enable mos-vis.service
sudo systemctl start mos-vis.service
```

## 8. 构建与开发

### 8.1 依赖安装（aarch64）

```bash
# 系统依赖
sudo apt update
sudo apt install -y build-essential cmake libspdlog-dev nlohmann-json3-dev \
  libboost-all-dev portaudio19-dev

# RKNN runtime（从 Rockchip 官方获取）
sudo dpkg -i rknn-runtime_1.6.0_arm64.deb

# sherpa-onnx C++ API（自行编译或预编译）
# 放置于项目 3rdparty 目录
```

### 8.2 VSCode + CMakeTools 配置

`.vscode/settings.json`：

```json
{
  "cmake.configureSettings": {
    "CMAKE_BUILD_TYPE": "Debug",
    "BUILD_SHARED_LIBS": "ON"
  },
  "cmake.buildDirectory": "${workspaceFolder}/build",
  "cmake.generator": "Unix Makefiles",
  "cmake.sourceDirectory": "${workspaceFolder}"
}
```

`.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Run mos_vis_server (dev)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build/bin/mos_vis_server",
      "args": ["-c", "./config/config.json", "-l", "debug"],
      "stopAtEntry": false,
      "cwd": "${workspaceFolder}",
      "environment": [],
      "externalConsole": false,
      "MIMode": "gdb"
    }
  ]
}
```

### 8.3 构建步骤

```bash
git clone <repository>
cd mos-vis
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
```

### 8.4 RKNN 检测与回退

```cmake
if(EXISTS "/usr/include/rknn_api.h" AND EXISTS "/usr/lib/librknnrt.so")
  add_library(rknnrt SHARED IMPORTED)
  set_target_properties(rknnrt PROPERTIES
    IMPORTED_LOCATION "/usr/lib/librknnrt.so"
    INTERFACE_INCLUDE_DIRECTORIES "/usr/include")
  set(MOS_VIS_HAS_RKNN ON)
else()
  set(MOS_VIS_HAS_RKNN OFF)
endif()
```

源代码中建议使用条件编译：

```cpp
#ifdef MOS_VIS_HAS_RKNN
#include "rknn_api.h"
// NPU 加速实现
#else
// CPU 回退或禁用
#endif
```

### 8.5 Sherpa-ONNX 检测与回退

```cmake
set(SHERPA_ONNX_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/3rdparty/include")
set(SHERPA_ONNX_LIB "${PROJECT_SOURCE_DIR}/3rdparty/lib/libsherpa-onnx-cxx-api.so")
if(EXISTS "${SHERPA_ONNX_INCLUDE_DIR}/sherpa-onnx/c-api/cxx-api.h" AND EXISTS "${SHERPA_ONNX_LIB}")
  add_library(sherpa_onnx_cxx_api SHARED IMPORTED)
  set_target_properties(sherpa_onnx_cxx_api PROPERTIES
    IMPORTED_LOCATION "${SHERPA_ONNX_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SHERPA_ONNX_INCLUDE_DIR}")
  set(MOS_VIS_HAS_SHERPA_ONNX ON)
else()
  set(MOS_VIS_HAS_SHERPA_ONNX OFF)
endif()
```

`3rdparty` 目录结构：

```text
${PROJECT_SOURCE_DIR}/3rdparty/
├── include/
│   └── sherpa-onnx/
│       └── c-api/
│           └── cxx-api.h
└── lib/
    └── libsherpa-onnx-cxx-api.so
```

## 9. 服务运行与维护

### 9.1 启动与状态查询

```bash
sudo systemctl start mos-vis
sudo systemctl status mos-vis
sudo journalctl -u mos-vis -f
```

### 9.2 日志管理

- `spdlog` 同时输出到 `journald` 与 `/var/log/mos_vis.log`
- 建议配合 `logrotate`

### 9.3 故障恢复

- `systemd` 自动重启：`Restart=always`
- `ControlClient` 断连后按配置自动重连
- 音频设备热插拔异常可由服务重启恢复

## 10. 接口定义

### 10.1 `ControlClient` WebSocket 协议

请求示例：

```json
{
  "id": "req-uuid",
  "action": "light.set",
  "params": {
    "brightness": 80,
    "zone": "living_room"
  }
}
```

响应示例：

```json
{
  "id": "req-uuid",
  "status": "ok",
  "message": "light brightness set to 80"
}
```

### 10.2 RAG 服务接口

- URL：`http://172.16.1.100:8081/rag`
- Method：`POST`
- Content-Type：`application/json`
- Request：`{"query": "今天天气怎么样"}`
- Response：`{"answer": "今天晴天，25°C"}`

服务不可用时降级回复：“知识服务暂时不可用”。

## 11. 性能与优化建议

- NPU 利用率：VAD/KWS/ASR 转 RKNN INT8 后单次推理可低于 10ms
- 内存占用：核心服务约 < 200MB（含音频缓冲与模型）
- 端到端延迟：唤醒 < 300ms，控制指令 < 1.5s（含网络）
- 音频缓冲：建议 `ring_seconds=10`，双缓冲避免丢帧

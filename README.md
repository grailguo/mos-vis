# mos-vis
voice interactive system

========================
[PROJECT STRUCTURE]
========================
The project contains:
1. a core library: mos_vis_core
2. an executable: mos_vis_server

Naming conventions (strict):
- namespace: mos::vis
- core class: VoiceInteractiveAgent
- class names: PascalCase
- file names: snake_case
- variables/functions: snake_case

(According to Google C++ Style, types use PascalCase and variables/functions use snake_case.)

========================
[SERVER REQUIREMENTS]
========================
mos_vis_server is a standard Ubuntu Linux systemd service.

Config loading rules:
- debug/develop: ./config/config.json
- release: /etc/mos_vis/config.json
- CLI override: --config <path>

Server responsibilities:
- parse CLI arguments
- resolve config path
- initialize spdlog
- load JSON config (nlohmann::json)
- create VoiceInteractiveAgent
- call Initialize(), Start()
- handle SIGINT/SIGTERM
- graceful shutdown

Dependencies:
- mos_vis_core
- spdlog
- nlohmann_json

========================
[CORE LIBRARY REQUIREMENTS]
========================
mos_vis_core depends on:
- spdlog
- nlohmann_json
- Boost.Beast (websocket client)
- PortAudio
- RKNN runtime:
  /usr/include/rknn_api.h
  /usr/lib/librknnrt.so
- sherpa-onnx C++ API:
  ${PROJECT_SOURCE_DIR}/3rdparty/include/sherpa-onnx/c-api/cxx-api.h
  ${PROJECT_SOURCE_DIR}/3rdparty/lib/libsherpa-onnx-cxx-api.so

========================
[CORE ARCHITECTURE]
========================
Expose:
mos::vis::VoiceInteractiveAgent

Responsibilities:
- accept AppConfig from server
- initialize modules
- manage pipelines
- provide:
  Initialize()
  Start()
  Stop()
  IsRunning()

========================
[MODULES]
========================

Audio:
- AudioInput
- AudioOutput
- PortAudioGuard (RAII)
- RingBuffer<float>

Engines:
- VadEngine
- KwsEngine
- AsrEngine (sherpa-onnx)
- NluEngine
- TtsEngine

Control:
- ControlClient (Boost.Beast websocket)
  - send JSON payload
  - async request/response

Pipelines:
- WakeupPipeline
- RecognitionPipeline
- ControlPipeline
- SpeakPipeline

========================
[VOICE FLOWS]
========================

1. Wakeup:
audio -> VAD -> KWS -> TTS(ack)

2. Device control:
audio -> VAD -> ASR -> NLU -> ControlClient(async JSON) -> TTS

3. Knowledge:
audio -> VAD -> ASR -> NLU -> RAG -> TTS

========================
[NLU OUTPUT]
========================

enum class IntentType {
  kUnknown,
  kWakeup,
  kDeviceControl,
  kKnowledgeQuery,
  kChat
};

struct IntentResult {
  IntentType intent;
  std::string text;
  std::string action;
  nlohmann::json slots;
};

========================
[THREAD MODEL]
========================
- audio capture thread
- processing thread (VAD/KWS/ASR)
- control async thread
- TTS playback thread

Use thread-safe queues / ring buffers.

========================
[CMAKE REQUIREMENTS]
========================
- root CMakeLists.txt
- src/CMakeLists.txt
- tests/CMakeLists.txt
- add_library(mos_vis_core)
- add_executable(mos_vis_server)
- proper target_link_libraries
- include directories correct
- optional alias: mos::vis_core

========================
[INSTALL + SYSTEMD]
========================
Install:
- binary → /usr/local/bin/mos_vis_server
- config → /etc/mos_vis/config.json
- service → /etc/systemd/system/mos-vis.service

systemd:
- Restart=always
- After=network.target sound.target

========================
[DESIGN RULES]
========================
- RAII everywhere
- no global state
- dependency injection friendly
- abstract interfaces for engines
- clean header/source separation
- minimal coupling
- compile without hardware

========================
[CODEX EXECUTION CONSTRAINTS]
========================

You MUST follow this workflow:

STEP 1 — PLAN FIRST
- Output a PLAN section
- Break project into modules
- Define dependency order
- Define minimal compilable milestone

STEP 2 — TEST STRATEGY
- Define what to test:
  - config loader
  - ring buffer
  - agent lifecycle
  - pipeline routing
  - control JSON payload
- Use GTest
- Tests must not depend on hardware

STEP 3 — IMPLEMENT IN ORDER
Follow strict order:
1. project skeleton (CMake)
2. config model + loader
3. logging bootstrap
4. ring buffer
5. PortAudioGuard
6. audio interfaces
7. agent skeleton
8. pipeline interfaces
9. engine interfaces
10. control client
11. stub implementations
12. systemd + install
13. tests

STEP 4 — TEST-DRIVEN DEVELOPMENT
- write tests before implementation when possible
- minimal implementation to pass tests
- keep build always working

STEP 5 — STUB FIRST
- if unsure: create stub
- ensure compile success
- no hardware dependency required

========================
[STRICT RULES]
========================
- NEVER output code before PLAN
- NEVER produce one large uncontrolled dump
- ALWAYS keep project compilable
- ALWAYS keep modules decoupled
- ALWAYS prefer stub over broken implementation
- NEVER depend on runtime audio device or RKNN for tests

========================
[OUTPUT FORMAT]
========================
You must output in this order:

1. PLAN
2. TEST STRATEGY
3. DIRECTORY TREE
4. ROOT CMakeLists.txt
5. src/CMakeLists.txt
6. tests/CMakeLists.txt
7. source files (in dependency order)

========================
[FINAL REQUIREMENT]
========================
Even if functionality is incomplete:
- the project MUST compile
- tests MUST run
- modules MUST be structured correctly
- missing parts must be marked TODO

Prioritize a clean, buildable, testable architecture over full implementation.

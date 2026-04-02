#include "mos/vis/core/voice_interactive_agent.h"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

namespace mos::vis {

VoiceInteractiveAgent::VoiceInteractiveAgent(AppConfig config, AgentDependencies dependencies)
    : config_(std::move(config)), dependencies_(std::move(dependencies)) {}

VoiceInteractiveAgent::~VoiceInteractiveAgent() { Stop(); }

bool VoiceInteractiveAgent::Initialize() {
  EnsureDefaultDependencies();
  initialized_.store(true);
  spdlog::info("VoiceInteractiveAgent initialized");
  return true;
}

bool VoiceInteractiveAgent::Start() {
  if (!initialized_.load()) {
    return false;
  }
  if (running_.exchange(true)) {
    return true;
  }

  dependencies_.audio_input->Start();
  dependencies_.audio_output->Start();

  capture_thread_ = std::thread(&VoiceInteractiveAgent::CaptureLoop, this);
  processing_thread_ = std::thread(&VoiceInteractiveAgent::ProcessingLoop, this);
  control_thread_ = std::thread(&VoiceInteractiveAgent::ControlLoop, this);
  tts_thread_ = std::thread(&VoiceInteractiveAgent::TtsLoop, this);

  spdlog::info("VoiceInteractiveAgent started");
  return true;
}

void VoiceInteractiveAgent::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  if (processing_thread_.joinable()) {
    processing_thread_.join();
  }
  if (control_thread_.joinable()) {
    control_thread_.join();
  }
  if (tts_thread_.joinable()) {
    tts_thread_.join();
  }

  if (dependencies_.audio_input) {
    dependencies_.audio_input->Stop();
  }
  if (dependencies_.audio_output) {
    dependencies_.audio_output->Stop();
  }

  spdlog::info("VoiceInteractiveAgent stopped");
}

void VoiceInteractiveAgent::ReloadConfig(const AppConfig& config) {
  std::lock_guard<std::mutex> lock(config_mutex_);
  config_ = config;
  spdlog::info("VoiceInteractiveAgent config reloaded");
}

bool VoiceInteractiveAgent::IsRunning() const { return running_.load(); }

void VoiceInteractiveAgent::EnsureDefaultDependencies() {
  if (!dependencies_.audio_input) {
    dependencies_.audio_input = std::make_shared<NullAudioInput>();
  }
  if (!dependencies_.audio_output) {
    dependencies_.audio_output = std::make_shared<NullAudioOutput>();
  }
  if (!dependencies_.vad_engine) {
    dependencies_.vad_engine = std::make_shared<StubVadEngine>();
  }
  if (!dependencies_.kws_engine) {
    dependencies_.kws_engine = std::make_shared<StubKwsEngine>();
  }
  if (!dependencies_.asr_engine) {
    dependencies_.asr_engine = std::make_shared<StubAsrEngine>();
  }
  if (!dependencies_.nlu_engine) {
    dependencies_.nlu_engine = std::make_shared<StubNluEngine>();
  }
  if (!dependencies_.tts_engine) {
    dependencies_.tts_engine = std::make_shared<StubTtsEngine>();
  }
  if (!dependencies_.control_client) {
    dependencies_.control_client = std::make_shared<BeastControlClient>(true);
  }
  if (!dependencies_.wakeup_pipeline) {
    dependencies_.wakeup_pipeline = std::make_shared<StubWakeupPipeline>();
  }
  if (!dependencies_.recognition_pipeline) {
    dependencies_.recognition_pipeline = std::make_shared<StubRecognitionPipeline>();
  }
  if (!dependencies_.control_pipeline) {
    dependencies_.control_pipeline = std::make_shared<StubControlPipeline>();
  }
  if (!dependencies_.speak_pipeline) {
    dependencies_.speak_pipeline = std::make_shared<StubSpeakPipeline>();
  }
}

void VoiceInteractiveAgent::CaptureLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void VoiceInteractiveAgent::ProcessingLoop() {
  PipelineRouter router;
  while (running_.load()) {
    AppConfig config_snapshot;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      config_snapshot = config_;
    }

    const std::string text = dependencies_.recognition_pipeline->Run();
    const IntentResult intent = dependencies_.nlu_engine->Parse(text);
    const std::string route = router.Route(intent);
    if (route == "wakeup") {
      dependencies_.wakeup_pipeline->Run();
    } else if (route == "control") {
      dependencies_.control_pipeline->Run(intent);
      auto future = dependencies_.control_client->SendJsonAsync(
          {{"device_id", config_snapshot.device_id}, {"action", intent.action}});
      (void)future.get();
    } else {
      dependencies_.speak_pipeline->Run(intent.text);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void VoiceInteractiveAgent::ControlLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void VoiceInteractiveAgent::TtsLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace mos::vis

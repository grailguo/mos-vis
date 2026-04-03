
#include "mos/vis/agent/voice_interactive_agent.h"

#include <chrono>
#include <thread>

namespace mos::vis {

VoiceInteractiveAgent::VoiceInteractiveAgent(AppConfig config)
    : config_(std::move(config)) {}

Status VoiceInteractiveAgent::Initialize() {
  const auto capacity =
      static_cast<std::size_t>(config_.audio.sample_rate * config_.audio.ring_seconds);
  ring_ = std::make_shared<AudioRingBuffer>(capacity);
  capture_ = std::make_unique<AudioCapture>(config_.audio, ring_);
  controller_ = std::make_unique<SessionController>(config_, ring_);
  return controller_->Initialize();
}

Status VoiceInteractiveAgent::Run() {
  Status st = capture_->Start();
  if (!st.ok()) {
    return st;
  }

  running_ = true;
  while (running_) {
    controller_->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return Status::Ok();
}

void VoiceInteractiveAgent::Stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  if (capture_ != nullptr) {
    capture_->Stop();
  }
}

}  // namespace mos::vis

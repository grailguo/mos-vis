
#pragma once
#include <memory>
#include "mos/vis/audio/audio_capture.h"
#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"
#include "mos/vis/runtime/session_controller.h"

namespace mos::vis {

class VoiceInteractiveAgent {
 public:
  explicit VoiceInteractiveAgent(AppConfig config);

  Status Initialize();
  Status Run();
  void Stop();

 private:
  AppConfig config_;
  std::shared_ptr<AudioRingBuffer> ring_;
  std::unique_ptr<AudioCapture> capture_;
  std::unique_ptr<SessionController> controller_;
  bool running_ = false;
};

}  // namespace mos::vis

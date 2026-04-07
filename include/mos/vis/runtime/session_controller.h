
#pragma once
#include <deque>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/asr/asr_engine.h"
#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"
#include "mos/vis/control/control_engine.h"
#include "mos/vis/kws/kws_engine.h"
#include "mos/vis/nlu/nlu_engine.h"
#include "mos/vis/runtime/pipeline_scheduler.h"
#include "mos/vis/runtime/session_context.h"
#include "mos/vis/runtime/session_state.h"
#include "mos/vis/runtime/stage_factory.h"
#include "mos/vis/tts/tts_engine.h"
#include "mos/vis/vad/vad_engine.h"

namespace mos::vis {

class SessionController {
 public:

  SessionController(const AppConfig& config, std::shared_ptr<AudioRingBuffer> ring);

  Status Initialize();
  void Tick();
  SessionState state() const;

 private:

  std::string ResolveWakeAckText(const std::string& keyword) const;
  std::string ResolveWakeAckPresetFile(const std::string& keyword) const;
  void HandleKwsHit(const std::string& keyword, const std::string& detail_json);

  AppConfig config_;
  std::shared_ptr<AudioRingBuffer> ring_;
  SessionContext context_;
  PipelineScheduler scheduler_;
  std::shared_ptr<VadEngine> vad1_;
  std::shared_ptr<VadEngine> vad2_;
  std::shared_ptr<KwsEngine> kws_;
  std::shared_ptr<AsrEngine> asr_;
  std::shared_ptr<NluEngine> nlu_;
  std::shared_ptr<ControlEngine> control_;
  std::shared_ptr<TtsEngine> tts_;
};

}  // namespace mos::vis

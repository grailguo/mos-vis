#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "mos/vis/audio/audio_input.h"
#include "mos/vis/audio/audio_output.h"
#include "mos/vis/audio/port_audio_guard.h"
#include "mos/vis/config/app_config.h"
#include "mos/vis/control/control_client.h"
#include "mos/vis/engines/asr_engine.h"
#include "mos/vis/engines/kws_engine.h"
#include "mos/vis/engines/nlu_engine.h"
#include "mos/vis/engines/tts_engine.h"
#include "mos/vis/engines/vad_engine.h"
#include "mos/vis/pipeline/pipeline_router.h"
#include "mos/vis/pipeline/pipelines.h"

namespace mos::vis {

struct AgentDependencies {
  std::shared_ptr<AudioInput> audio_input;
  std::shared_ptr<AudioOutput> audio_output;
  std::shared_ptr<VadEngine> vad_engine;
  std::shared_ptr<KwsEngine> kws_engine;
  std::shared_ptr<AsrEngine> asr_engine;
  std::shared_ptr<NluEngine> nlu_engine;
  std::shared_ptr<TtsEngine> tts_engine;
  std::shared_ptr<ControlClient> control_client;
  std::shared_ptr<WakeupPipeline> wakeup_pipeline;
  std::shared_ptr<RecognitionPipeline> recognition_pipeline;
  std::shared_ptr<ControlPipeline> control_pipeline;
  std::shared_ptr<SpeakPipeline> speak_pipeline;
};

class VoiceInteractiveAgent {
 public:
  explicit VoiceInteractiveAgent(AppConfig config, AgentDependencies dependencies = {});
  ~VoiceInteractiveAgent();

  bool Initialize();
  bool Start();
  void Stop();
  void ReloadConfig(const AppConfig& config);
  bool IsRunning() const;

 private:
  void EnsureDefaultDependencies();

  void CaptureLoop();
  void ProcessingLoop();
  void ControlLoop();
  void TtsLoop();

  AppConfig config_;
  AgentDependencies dependencies_;
  PortAudioGuard port_audio_guard_;

  std::atomic<bool> initialized_{false};
  std::atomic<bool> running_{false};

  std::thread capture_thread_;
  std::thread processing_thread_;
  std::thread control_thread_;
  std::thread tts_thread_;

  mutable std::mutex config_mutex_;
};

}  // namespace mos::vis

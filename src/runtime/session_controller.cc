#include "mos/vis/runtime/session_controller.h"

#include "mos/vis/common/logging.h"
#include "mos/vis/vad/vad_engine.h"
#include "mos/vis/kws/kws_engine.h"
#include "mos/vis/asr/asr_engine.h"
#include "mos/vis/nlu/nlu_engine.h"
#include "mos/vis/control/control_engine.h"
#include "mos/vis/tts/tts_engine.h"

namespace mos::vis {

SessionController::SessionController(const AppConfig& config,
                                     std::shared_ptr<AudioRingBuffer> ring)
    : config_(config),
      ring_(std::move(ring)),
      context_(config_, ring_) {
  // No need to initialize audio readers – each stage creates its own.
  // No need to initialize VAD state machines – stages will do that in OnAttach.
}

Status SessionController::Initialize() {
  // Create engine instances using the global factory functions
  vad1_ = CreateVadEngine();
  vad2_ = CreateVadEngine();
  kws_  = CreateKwsEngine();
  asr_  = CreateAsrEngine();
  nlu_  = CreateNluEngine();
  control_ = CreateControlEngine();
  tts_  = CreateTtsEngine();

  // Initialize engines that are enabled in config
  if (config_.vad1.enabled && vad1_) {
    Status st = vad1_->Initialize(config_.vad1);
    if (!st.ok()) {
      GetLogger()->error("VAD‑1 engine initialization failed: {}", st.message());
      return st;
    }
  }
  if (config_.vad2.enabled && vad2_) {
    Status st = vad2_->Initialize(config_.vad2);
    if (!st.ok()) {
      GetLogger()->error("VAD‑2 engine initialization failed: {}", st.message());
      return st;
    }
  }
  if (config_.kws.enabled && kws_) {
    Status st = kws_->Initialize(config_.kws);
    if (!st.ok()) {
      GetLogger()->error("KWS engine initialization failed: {}", st.message());
      return st;
    }
  }
  if (config_.asr.enabled && asr_) {
    Status st = asr_->Initialize(config_.asr);
    if (!st.ok()) {
      GetLogger()->error("ASR engine initialization failed: {}", st.message());
      return st;
    }
  }
  if (config_.nlu.enabled && nlu_) {
    Status st = nlu_->Initialize(config_.nlu);
    if (!st.ok()) {
      GetLogger()->error("NLU engine initialization failed: {}", st.message());
      return st;
    }
  }
  if (config_.control.enabled && control_) {
    Status st = control_->Initialize(config_.control);
    if (!st.ok()) {
      GetLogger()->error("Control engine initialization failed: {}", st.message());
      return st;
    }
  }
  if (config_.tts.enabled && tts_) {
    Status st = tts_->Initialize(config_.tts, config_.audio);
    if (!st.ok()) {
      GetLogger()->error("TTS engine initialization failed: {}", st.message());
      return st;
    }
  }

  // Share engine instances with the pipeline stages via SessionContext
  context_.vad1 = vad1_;
  context_.vad2 = vad2_;
  context_.kws = kws_;
  context_.asr = asr_;
  context_.nlu = nlu_;
  context_.control = control_;
  context_.tts = tts_;
  context_.ring_buffer = ring_;

  // Create pipeline stages and attach them to the scheduler
  Status st = StageFactory::CreateStages(&scheduler_, context_);
  if (!st.ok()) {
    GetLogger()->error("Pipeline stage creation failed: {}", st.message());
    return st;
  }

  GetLogger()->info("SessionController initialized with pipeline architecture");
  return Status::Ok();
}

void SessionController::Tick() {
  // Delegate to the pipeline scheduler
  Status st = scheduler_.Process(context_);
  if (!st.ok()) {
    GetLogger()->error("Pipeline tick failed: {}", st.message());
  }
}

SessionState SessionController::state() const {
  return context_.state;
}

}  // namespace mos::vis
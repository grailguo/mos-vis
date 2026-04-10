#include "mos/vis/runtime/session_controller.h"

#include "mos/vis/common/logging.h"
#include <vector>
#include "mos/vis/vad/vad_engine.h"
#include "mos/vis/kws/kws_engine.h"
#include "mos/vis/asr/asr_engine.h"
#include "mos/vis/nlu/nlu_engine.h"
#include "mos/vis/control/control_engine.h"
#include "mos/vis/tts/tts_engine.h"
#include "mos/vis/runtime/state_machine_controller.h"

namespace mos::vis {

namespace {
LogContext MakeLogCtx(const SessionContext& context, const char* module) {
  LogContext ctx;
  ctx.module = module;
  ctx.session = context.session_id;
  ctx.turn = context.turn_id;
  ctx.state = std::to_string(static_cast<int>(context.state));
  ctx.req = context.current_control_request_id;
  return ctx;
}

std::vector<std::string> CollectWakeAckTexts(const AppConfig& config) {
  std::vector<std::string> texts;
  texts.reserve(config.wake_ack_text.size());
  for (const auto& rule : config.wake_ack_text) {
    // If preset_file is configured, wake ACK should play that file directly.
    // Skip text preload to avoid unnecessary TTS generation.
    if (!rule.preset_file.empty()) {
      continue;
    }
    if (!rule.reply_text.empty()) {
      texts.push_back(rule.reply_text);
    }
  }
  return texts;
}

std::vector<std::string> CollectControlFixedTexts() {
  std::vector<std::string> texts;
  texts.reserve(5);
  texts.push_back("好的，校准已开始。请稍候...");
  texts.push_back("好的，分析已开始。请稍候...");
  texts.push_back("好的，已发送停止分析指令。");
  texts.push_back("校准完成。");
  texts.push_back("分析完成。");
  return texts;
}

std::vector<std::string> CollectTtsPreloadTexts(const AppConfig& config) {
  std::vector<std::string> texts = CollectWakeAckTexts(config);
  std::vector<std::string> control_texts = CollectControlFixedTexts();
  texts.insert(texts.end(), control_texts.begin(), control_texts.end());
  return texts;
}
}  // namespace

SessionController::SessionController(const AppConfig& config,
                                     std::shared_ptr<AudioRingBuffer> ring)
    : config_(config),
      ring_(std::move(ring)),
      context_(config_) {
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
  control_ = CreateControlEngine(config_.control);
  tts_  = CreateTtsEngine();

  // Initialize engines that are enabled in config
  if (config_.vad1.enabled && vad1_) {
    Status st = vad1_->Initialize(config_.vad1);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "vad1_init_failed"), Kv("err", st.message())});
      return st;
    }
  }
  if (config_.vad2.enabled && vad2_) {
    Status st = vad2_->Initialize(config_.vad2);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "vad2_init_failed"), Kv("err", st.message())});
      return st;
    }
  }
  if (config_.kws.enabled && kws_) {
    Status st = kws_->Initialize(config_.kws);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "kws_init_failed"), Kv("err", st.message())});
      return st;
    }
  }
  if (config_.asr.enabled && asr_) {
    Status st = asr_->Initialize(config_.asr);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "asr_init_failed"), Kv("err", st.message())});
      return st;
    }
  }
  if (config_.nlu.enabled && nlu_) {
    Status st = nlu_->Initialize(config_.nlu);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "nlu_init_failed"), Kv("err", st.message())});
      return st;
    }
  }
  if (config_.control.enabled && control_) {
    Status st = control_->Initialize();
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "control_init_failed"), Kv("err", st.message())});
      return st;
    }
  }
  if (config_.tts.enabled && tts_) {
    Status st = tts_->Initialize(config_.tts, config_.audio);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
               {Kv("result", "tts_init_failed"), Kv("err", st.message())});
      return st;
    }
    if (config_.tts.fixed_phrase_cache) {
      const std::vector<std::string> preload_texts = CollectTtsPreloadTexts(config_);
      st = tts_->PreloadFixedPhrases(preload_texts);
      if (!st.ok()) {
        LogWarn(logevent::kSystemBoot, MakeLogCtx(context_, "SessionController"),
                {Kv("detail", "tts_preload_fixed_phrases_failed"),
                 Kv("err", st.message())});
      }
    }
  }

  // Initialize state machine controller (v3 architecture)
  context_.state_machine = std::make_unique<StateMachineController>(context_);

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
    LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
             {Kv("result", "pipeline_stage_create_failed"), Kv("err", st.message())});
    return st;
  }

  LogInfo(logevent::kSystemBoot, MakeLogCtx(context_, "SessionController"),
          {Kv("detail", "initialized_with_pipeline_architecture")});

  if (!startup_beep_played_ && config_.tts.enabled && tts_ != nullptr) {
    startup_beep_played_ = true;
    Status beep_st = tts_->PlayTone(/*frequency_hz=*/1000, /*duration_ms=*/700, /*amplitude=*/0.22F);
    if (!beep_st.ok()) {
      LogWarn(logevent::kSystemBoot, MakeLogCtx(context_, "SessionController"),
              {Kv("detail", "startup_beep_failed"), Kv("err", beep_st.message())});
    }
  }

  return Status::Ok();
}

void SessionController::Tick() {
  // Delegate to the pipeline scheduler
  Status st = scheduler_.Process(context_);
  if (!st.ok()) {
    LogError(logevent::kSessionEnd, MakeLogCtx(context_, "SessionController"),
             {Kv("result", "pipeline_tick_failed"), Kv("err", st.message())});
  }

  // Process state machine events (v3 architecture)
  if (context_.state_machine) {
    context_.state_machine->Tick();
  }
}

SessionState SessionController::state() const {
  return context_.state;
}

}  // namespace mos::vis

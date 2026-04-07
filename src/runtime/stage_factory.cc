#include "mos/vis/runtime/stage_factory.h"

#include "mos/vis/runtime/stages/control_notification_stage.h"
#include "mos/vis/runtime/stages/vad1_stage.h"
#include "mos/vis/runtime/stages/kws_stage.h"
#include "mos/vis/runtime/stages/vad2_stage.h"
#include "mos/vis/runtime/stages/asr_stage.h"
#include "mos/vis/runtime/stages/recognizing_stage.h"
#include "mos/vis/runtime/stages/executing_stage.h"
#include "mos/vis/runtime/stages/tts_stage.h"

namespace mos::vis {

Status StageFactory::CreateStages(PipelineScheduler* scheduler, SessionContext& context) {
  if (scheduler == nullptr) {
    return Status::InvalidArgument("StageFactory: scheduler is null");
  }

  // Create stages in the order they should be executed each tick.
  // This matches the original SessionController::Tick() order:
  // 1. ControlNotificationStage
  // 2. Vad1Stage
  // 3. KwsStage
  // 4. Vad2Stage
  // 5. RecognizingStage (but note: original order is Vad2, Recognizing, Executing, ASR, TTS)
  // 6. ExecutingStage
  // 7. AsrStage
  // 8. TtsStage
  // However, note that ASR stage runs after Vad2 stage (speech detection) and before TTS.
  // We'll follow the original tick order as closely as possible.

  scheduler->AddStage(std::make_unique<ControlNotificationStage>());
  scheduler->AddStage(std::make_unique<Vad1Stage>());
  scheduler->AddStage(std::make_unique<KwsStage>());
  scheduler->AddStage(std::make_unique<Vad2Stage>());
  scheduler->AddStage(std::make_unique<AsrStage>());
  scheduler->AddStage(std::make_unique<RecognizingStage>());
  scheduler->AddStage(std::make_unique<ExecutingStage>());
  scheduler->AddStage(std::make_unique<TtsStage>());

  // Initialize the scheduler with the context (calls OnAttach for each stage)
  return scheduler->Initialize(context);
}

}  // namespace mos::vis
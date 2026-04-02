#include "mos/vis/pipeline/pipeline_router.h"

#include "mos/vis/pipeline/pipelines.h"

#include <spdlog/spdlog.h>

namespace mos::vis {

std::string PipelineRouter::Route(const IntentResult& intent) const {
  switch (intent.intent) {
    case IntentType::kWakeup:
      return "wakeup";
    case IntentType::kDeviceControl:
      return "control";
    case IntentType::kKnowledgeQuery:
    case IntentType::kChat:
      return "speak";
    case IntentType::kUnknown:
    default:
      return "recognition";
  }
}

void StubWakeupPipeline::Run() { spdlog::info("Wakeup pipeline triggered"); }

std::string StubRecognitionPipeline::Run() { return "stub recognition text"; }

void StubControlPipeline::Run(const IntentResult& intent) {
  spdlog::info("Control pipeline action: {}", intent.action);
}

void StubSpeakPipeline::Run(const std::string& text) {
  spdlog::info("Speak pipeline text: {}", text);
}

}  // namespace mos::vis

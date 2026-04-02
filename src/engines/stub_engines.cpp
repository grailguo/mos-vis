#include "mos/vis/engines/asr_engine.h"
#include "mos/vis/engines/kws_engine.h"
#include "mos/vis/engines/nlu_engine.h"
#include "mos/vis/engines/tts_engine.h"
#include "mos/vis/engines/vad_engine.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace mos::vis {

bool StubVadEngine::IsSpeechFrame(const float* frame, int size) {
  if (frame == nullptr || size <= 0) {
    return false;
  }
  for (int i = 0; i < size; ++i) {
    if (frame[i] != 0.0f) {
      return true;
    }
  }
  return false;
}

bool StubKwsEngine::Detect(const std::string& text) {
  return text.find("wake") != std::string::npos || text.find("hello") != std::string::npos;
}

std::string StubAsrEngine::Transcribe(const float* audio, int size) {
  if (audio == nullptr || size <= 0) {
    return {};
  }
  return "stub transcript";
}

IntentResult StubNluEngine::Parse(const std::string& text) {
  IntentResult result;
  result.text = text;

  if (text.find("wake") != std::string::npos) {
    result.intent = IntentType::kWakeup;
    result.action = "ack";
  } else if (text.find("light") != std::string::npos || text.find("device") != std::string::npos) {
    result.intent = IntentType::kDeviceControl;
    result.action = "device_control";
  } else if (text.find("what") != std::string::npos || text.find("who") != std::string::npos) {
    result.intent = IntentType::kKnowledgeQuery;
    result.action = "knowledge_query";
  } else if (!text.empty()) {
    result.intent = IntentType::kChat;
    result.action = "chat";
  }

  return result;
}

void StubTtsEngine::Speak(const std::string& text) { spdlog::info("TTS: {}", text); }

}  // namespace mos::vis

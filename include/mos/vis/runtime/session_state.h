
#pragma once
namespace mos::vis {
enum class SessionState {
  kIdle = 0,
  kWakeCandidate,
  kPreListening,
  kListening,
  kFinalizing,
  kRecognizing,
  kExecuting,
  kThinking,
  kSpeaking,
};
}  // namespace mos::vis

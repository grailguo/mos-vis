
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
  kSpeaking,
};
}  // namespace mos::vis

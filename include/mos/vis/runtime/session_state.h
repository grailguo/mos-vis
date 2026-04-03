
#pragma once
namespace mos::vis {
enum class SessionState {
  kIdle = 0,
  kWakeCandidate,
  kPreListening,
  kListening,
  kFinalizing,
  kThinking,
  kSpeaking,
};
}  // namespace mos::vis

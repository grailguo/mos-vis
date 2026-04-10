
#pragma once
namespace mos::vis {
enum class SessionState {
  // --- Original 8 states (v1/v2) ---
  kIdle = 0,           // v3: kWaitingWakeup
  kWakeCandidate,      // v3: kWakeDetecting
  kPreListening,       // v3: kAckSpeaking + kWaitingCommandSpeech
  kListening,          // v3: part of kRecognizingCommand
  kFinalizing,         // v3: part of kRecognizingCommand
  kRecognizing,        // v3: kUnderstandingIntent
  kExecuting,          // v3: kExecutingControlSync + kWaitingControlAsync
  kSpeaking,           // v3: kResultSpeaking

  // --- New states for v3 architecture ---
  kBooting,            // System boot initialization
  kAckSpeaking,        // Playing wake acknowledgment
  kWaitingCommandSpeech, // Waiting for command speech after wake
  kRecognizingCommand, // Recognizing command speech (ASR active)
  kUnderstandingIntent, // NLU processing
  kExecutingControlSync, // Synchronous control execution
  kWaitingControlAsync, // Waiting for asynchronous control completion
  kQueryingRag,        // RAG query processing
  kChattingLlm,        // LLM chat processing
  kErrorRecovery,      // Error recovery state
  kShuttingDown,       // System shutdown in progress

  // Special state for transition matching (not a runtime state)
  kAny,
};

/**
 * @brief Map v3 state to equivalent v2 state for backward compatibility.
 *
 * Pipeline stages that still operate on the original 8-state model should
 * call this function to convert the current v3 state to its closest v2
 * equivalent before making decisions.
 */
SessionState MapV3ToV2State(SessionState v3_state);

}  // namespace mos::vis

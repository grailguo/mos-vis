#pragma once

#include <cstdint>
#include <string>

namespace mos::vis {

// Forward declaration for dependency resolution
struct VisEvent;

/**
 * @brief Event types for the v3 state machine architecture.
 *
 * This enumeration defines all possible events that can trigger state transitions
 * in the centralized state machine controller. Events are produced by pipeline
 * stages and consumed by the StateMachineController.
 */
enum class VisEventType {
  // ===== Lifecycle Events =====
  kBootCompleted,      // System boot completed successfully
  kBootFailed,         // System boot failed
  kShutdownRequested,  // System shutdown requested
  kFatalError,         // Fatal error requiring recovery

  // ===== Wake / KWS Events =====
  kWakeSpeechDetected, // VAD detected speech in wake detection window
  kKwsMatched,         // KWS engine matched a wake word
  kKwsRejected,        // KWS engine rejected a candidate (not a wake word)
  kKwsTimeout,         // KWS detection window timed out

  // ===== Command Window Events =====
  kCommandSpeechDetected, // VAD detected speech in command waiting window
  kCommandSpeechEnd,      // VAD detected speech end in command recognition window
  kCommandWaitTimeout,    // Command waiting window timed out (15 seconds)

  // ===== ASR Events =====
  kAsrPartialResult,         // ASR produced a partial recognition result
  kAsrFinalResult,           // ASR produced a final recognition result
  kAsrEndpointWithEmptyText, // ASR endpoint detected but text is empty
  kAsrTimeout,               // ASR recognition timed out
  kAsrError,                 // ASR engine encountered an error
  kUserBargeIn,              // User started speaking during TTS playback

  // ===== NLU Events =====
  kIntentControl,    // NLU identified a control intent
  kIntentRag,        // NLU identified a RAG query intent
  kIntentLlm,        // NLU identified a LLM chat intent
  kIntentUnknown,    // NLU could not identify intent
  kNluError,         // NLU engine encountered an error
  kNluTimeout,       // NLU processing timed out

  // ===== Control Events =====
  kControlSyncAckSuccess,     // Synchronous control acknowledgment succeeded
  kControlSyncAckFail,        // Synchronous control acknowledgment failed
  kControlSyncAckTimeout,     // Synchronous control acknowledgment timed out
  kControlTransportError,     // Control transport layer error
  kControlAsyncSuccess,       // Asynchronous control operation succeeded
  kControlAsyncFail,          // Asynchronous control operation failed
  kControlAsyncTimeout,       // Asynchronous control operation timed out
  kControlTransportDisconnected, // Control transport disconnected
  kControlAsyncStaleOrMismatched, // Async event with mismatched request ID

  // ===== RAG / LLM Events =====
  kRagSuccess,   // RAG query succeeded with results
  kRagEmpty,     // RAG query succeeded but returned empty results
  kRagTimeout,   // RAG query timed out
  kRagError,     // RAG engine encountered an error
  kLlmSuccess,   // LLM chat succeeded with response
  kLlmTimeout,   // LLM chat timed out
  kLlmError,     // LLM engine encountered an error

  // ===== TTS Events =====
  kTtsPlaybackCompleted, // TTS playback completed successfully
  kTtsPlaybackFailed,    // TTS playback failed

  // ===== Error Recovery Events =====
  kRecoverySucceeded, // Error recovery succeeded
  kRecoveryFailed,    // Error recovery failed
};

/**
 * @brief Rich event structure for v3 state machine.
 *
 * Contains all metadata needed for state transitions, guard evaluation,
 * and action execution. Events are produced by pipeline stages and
 * consumed by the StateMachineController.
 */
struct VisEvent {
  VisEventType type;
  std::string text;           // ASR text, NLU intent, reply text, etc.
  std::string request_id;     // For matching async operation responses
  std::string source_stage;   // Identifier of the producing stage
  int64_t session_id;         // Session identifier
  int32_t turn_id;            // Turn identifier within session
  std::string payload;        // JSON extension payload for complex data
  int64_t ts_ms;              // Timestamp in milliseconds

  // Helper constructors for common event patterns
  explicit VisEvent(VisEventType type) : type(type), ts_ms(0) {}

  VisEvent(VisEventType type, const std::string& text)
      : type(type), text(text), ts_ms(0) {}

  VisEvent(VisEventType type, const std::string& text,
           const std::string& request_id)
      : type(type), text(text), request_id(request_id), ts_ms(0) {}

  VisEvent(VisEventType type, const std::string& text,
           const std::string& request_id, const std::string& source_stage)
      : type(type), text(text), request_id(request_id),
        source_stage(source_stage), ts_ms(0) {}
};

}  // namespace mos::vis
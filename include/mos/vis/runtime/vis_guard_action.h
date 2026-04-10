#pragma once

namespace mos::vis {

/**
 * @brief Guard conditions for state transitions.
 *
 * Guards are boolean conditions evaluated during transition matching.
 * A transition only fires if its guard evaluates to true.
 */
enum class GuardType {
  // ===== Basic Guards =====
  kAlways,  // Always passes (default guard)

  // ===== Wake-related Guards =====
  kNotInWakeCooldown,  // System is not in wake cooldown period
  kInWakeCooldown,     // System is in wake cooldown period

  // ===== ASR-related Guards =====
  kAsrFinalTextNonEmpty,  // ASR final result has non-empty text
  kAsrFinalTextEmpty,     // ASR final result has empty text

  // ===== Session Policy Guards =====
  kFallbackLlmEnabled,   // LLM fallback is enabled in configuration
  kFallbackLlmDisabled,  // LLM fallback is disabled
  kKeepSessionOpen,      // Session should remain open after this turn
  kNotKeepSessionOpen,   // Session should close after this turn
  kBargeInEnabled,       // User barge-in is enabled
  kBargeInDisabled,      // User barge-in is disabled

  // ===== Async Operation Guards =====
  kControlAsyncEventMatchesCurrent,    // Async event matches current request ID
  kControlAsyncEventNotMatchesCurrent, // Async event doesn't match current request ID
  kRagAsyncEventMatchesCurrent,        // RAG async event matches current request ID
  kLlmAsyncEventMatchesCurrent,        // LLM async event matches current request ID

  // ===== Error Handling Guards =====
  kErrorRecoverable,    // Error is recoverable
  kErrorNonRecoverable, // Error is non-recoverable

  // ===== Configuration Guards =====
  kControlEnabled,      // Control engine is enabled
  kRagEnabled,          // RAG engine is enabled
  kLlmEnabled,          // LLM engine is enabled
};

/**
 * @brief Actions to execute during state transitions.
 *
 * Actions are side effects performed when a transition fires.
 * They typically update context, schedule timeouts, or trigger external operations.
 */
enum class ActionType {
  // ===== No-op / Default =====
  kNoop,  // No operation

  // ===== Boot / Shutdown / Recovery Actions =====
  kInitDone,             // System initialization completed
  kRecordBootError,      // Record boot failure error
  kRunErrorRecovery,     // Initiate error recovery procedure
  kEnterShuttingDown,    // Begin system shutdown sequence

  // ===== Wake / KWS Actions =====
  kOpenKwsWindow,        // Open KWS detection window
  kCleanupKwsWindow,     // Clean up KWS window state
  kPlayAck,              // Play wake acknowledgment ("我在，请说。")

  // ===== Command Window / Session Actions =====
  kEnterWaitingCommandWindow,  // Enter command waiting window (start 15s timer)
  kEndSessionToWakeup,         // End session and return to wakeup waiting
  kEndSessionOrRearmCommandWait, // End session or rearm command wait based on policy

  // ===== ASR / NLU Actions =====
  kStartAsrForCommand,         // Start ASR engine for command recognition
  kTriggerAsrFinalization,     // Trigger ASR finalization on speech end
  kUpdateAsrPartial,           // Update ASR partial result in context
  kSubmitAsrFinalToNlu,        // Submit ASR final result to NLU
  kCleanupAsrAndRearmCommandWait, // Clean up ASR state and rearm command wait
  kPrepareReplyAsrRetry,       // Prepare retry reply ("没听清，请再说一次。")
  kPrepareReplyUnknown,        // Prepare unknown intent reply

  // ===== Control Actions =====
  kSendControlRequest,         // Send control request to device
  kStartWaitingAsyncNotify,    // Start waiting for async control notification
  kPrepareReplyControlFail,    // Prepare control failure reply
  kPrepareReplyControlTimeout, // Prepare control timeout reply
  kPrepareReplyControlTransportError, // Prepare transport error reply
  kPrepareReplyControlDone,    // Prepare control completion reply
  kDiscardStaleControlAsyncEvent, // Discard stale async control event

  // ===== RAG Actions =====
  kStartRagQuery,              // Start RAG query
  kPrepareReplyQueryResult,    // Prepare RAG query result reply
  kPrepareReplyQueryEmpty,     // Prepare empty result reply
  kPrepareReplyQueryFail,      // Prepare RAG query failure reply

  // ===== LLM Actions =====
  kStartLlmChat,               // Start LLM chat
  kPrepareReplyLlmResult,      // Prepare LLM chat result reply
  kPrepareReplyLlmFail,        // Prepare LLM chat failure reply

  // ===== TTS Actions =====
  kPlayReply,                  // Play TTS reply
  kStopCurrentTtsAndStartAsr,  // Stop current TTS and start ASR (barge-in)

  // ===== Timeout Actions =====
  kStartCommandWaitTimeout,    // Start command wait timeout (15s)
  kCancelCommandWaitTimeout,   // Cancel command wait timeout
  kStartAsrTimeout,            // Start ASR recognition timeout
  kCancelAsrTimeout,           // Cancel ASR recognition timeout
  kStartControlSyncTimeout,    // Start synchronous control timeout
  kCancelControlSyncTimeout,   // Cancel synchronous control timeout
  kStartControlAsyncTimeout,   // Start asynchronous control timeout
  kCancelControlAsyncTimeout,  // Cancel asynchronous control timeout

  // ===== Context Update Actions =====
  kUpdateKeepSessionOpen,      // Update keep_session_open flag
  kIncrementTurnId,            // Increment turn ID
  kClearCurrentRequestId,      // Clear current request ID
  kSetCurrentRequestId,        // Set current request ID
};

}  // namespace mos::vis
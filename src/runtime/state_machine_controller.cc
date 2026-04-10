#include "mos/vis/runtime/state_machine_controller.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <string>
#include <utility>

#include "mos/vis/common/logging.h"
#include "mos/vis/runtime/session_state.h"
#include "mos/vis/runtime/vis_event.h"
#include "mos/vis/runtime/vis_guard_action.h"
#include "mos/vis/runtime/subsm/transition_map.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

namespace {

// ============================================================================
// Transition Table for v3 State Machine
// ============================================================================
// ============================================================================
// Helper Functions
// ============================================================================

std::string StateToString(SessionState state) {
  switch (state) {
    case SessionState::kIdle: return "kIdle (kWaitingWakeup)";
    case SessionState::kWakeCandidate: return "kWakeCandidate (kWakeDetecting)";
    case SessionState::kPreListening: return "kPreListening";
    case SessionState::kListening: return "kListening";
    case SessionState::kFinalizing: return "kFinalizing";
    case SessionState::kRecognizing: return "kRecognizing";
    case SessionState::kExecuting: return "kExecuting";
    case SessionState::kSpeaking: return "kSpeaking";
    case SessionState::kBooting: return "kBooting";
    case SessionState::kAckSpeaking: return "kAckSpeaking";
    case SessionState::kWaitingCommandSpeech: return "kWaitingCommandSpeech";
    case SessionState::kRecognizingCommand: return "kRecognizingCommand";
    case SessionState::kUnderstandingIntent: return "kUnderstandingIntent";
    case SessionState::kExecutingControlSync: return "kExecutingControlSync";
    case SessionState::kWaitingControlAsync: return "kWaitingControlAsync";
    case SessionState::kQueryingRag: return "kQueryingRag";
    case SessionState::kChattingLlm: return "kChattingLlm";
    case SessionState::kErrorRecovery: return "kErrorRecovery";
    case SessionState::kShuttingDown: return "kShuttingDown";
    case SessionState::kAny: return "kAny (global)";
    default: return "Unknown";
  }
}

std::string EventTypeToString(VisEventType type) {
  // TODO: Implement full mapping
  return std::to_string(static_cast<int>(type));
}

std::string GuardTypeToString(GuardType guard) {
  // TODO: Implement full mapping
  return std::to_string(static_cast<int>(guard));
}

std::string ActionTypeToString(ActionType action) {
  // TODO: Implement full mapping
  return std::to_string(static_cast<int>(action));
}

// ============================================================================
// State Mapping for Backward Compatibility
// ============================================================================


// ============================================================================
// Guard Evaluation
// ============================================================================

bool EvaluateGuardImpl(GuardType guard, const VisEvent& event, SessionContext& context) {
  switch (guard) {
    case GuardType::kAlways:
      return true;

    case GuardType::kNotInWakeCooldown: {
      // Check if we're in wake cooldown period
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - context.last_wake_timestamp);
      return elapsed.count() >= context.subsm_state.wake_cooldown_ms;
    }

    case GuardType::kInWakeCooldown: {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - context.last_wake_timestamp);
      return elapsed.count() < context.subsm_state.wake_cooldown_ms;
    }

    case GuardType::kAsrFinalTextNonEmpty:
      return !event.text.empty();

    case GuardType::kAsrFinalTextEmpty:
      return event.text.empty();

    case GuardType::kKeepSessionOpen:
      return context.keep_session_open;

    case GuardType::kNotKeepSessionOpen:
      return !context.keep_session_open;

    case GuardType::kBargeInEnabled:
      return context.barge_in_enabled;

    case GuardType::kBargeInDisabled:
      return !context.barge_in_enabled;

    case GuardType::kControlAsyncEventMatchesCurrent:
      return event.request_id == context.current_control_request_id;

    case GuardType::kControlAsyncEventNotMatchesCurrent:
      return event.request_id != context.current_control_request_id;

    case GuardType::kControlEnabled:
      return context.config.control.enabled && context.control != nullptr;

    case GuardType::kRagEnabled:
      // TODO: Check RAG configuration
      return false;

    case GuardType::kLlmEnabled:
      // TODO: Check LLM configuration
      return false;

    case GuardType::kFallbackLlmEnabled:
      // TODO: Check fallback LLM configuration
      return false;

    case GuardType::kFallbackLlmDisabled:
      // TODO: Check fallback LLM configuration
      return true;

    case GuardType::kErrorRecoverable:
      // TODO: Determine if error is recoverable
      return true;

    case GuardType::kErrorNonRecoverable:
      // TODO: Determine if error is non-recoverable
      return false;

    default:
      // Unknown guard - treat as false for safety
      LogWarn(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "unknown_guard_type"),
               Kv("guard", static_cast<int>(guard))});
      return false;
  }
}

// ============================================================================
// Action Execution
// ============================================================================

void ExecuteActionImpl(ActionType action, const VisEvent& event, SessionContext& context, StateMachineController& controller) {
  // TODO: Implement all actions
  switch (action) {
    case ActionType::kNoop:
      break;

    case ActionType::kPlayAck:
      // Play wake acknowledgment "我在，请说。"
      {
        std::string keyword = event.text.empty() ? context.kws_state.last_wake_keyword : event.text;
        std::string ack_text = "我在，请说。"; // default
        std::string preset_file = "";
        if (!keyword.empty()) {
          // Resolve from configuration
          for (const auto& rule : context.config.wake_ack_text) {
            for (const auto& kw : rule.keywords) {
              if (kw == keyword) {
                ack_text = rule.reply_text;
                preset_file = rule.preset_file;
                break;
              }
            }
            if (!ack_text.empty()) break;
          }
        }
        // Cancel existing command wait timeout (if any) before playing ACK
        controller.CancelTimeout(StateMachineController::TimeoutType::kCommandWait);
        // Store for reference
        context.kws_state.last_wake_keyword = keyword;
        context.kws_state.last_wake_ack_text = ack_text;
        context.kws_state.last_wake_ack_preset_file = preset_file;
        context.last_wake_timestamp = std::chrono::steady_clock::now();
        // Queue TTS task
        context.tts_state.tasks.push_back(TtsTask{preset_file, ack_text});
        context.tts_state.wake_ack_pending = true;
        LogInfo(logevent::kStateTransition,
                LogContext{.module = "StateMachineController"},
                {Kv("detail", "action_play_ack"),
                 Kv("keyword", keyword),
                 Kv("text", ack_text)});
      }
      break;

    case ActionType::kEnterWaitingCommandWindow:
      // Start 15-second command wait timeout
      controller.StartTimeout(StateMachineController::TimeoutType::kCommandWait, 15000);
      // Clear TTS started flag
      context.reply_tts_started = false;
      // If we were in speaking state (reply TTS completed), increment turn ID for next command
      if (context.state == SessionState::kSpeaking) {
        ++context.turn_id;
        LogInfo(logevent::kStateTransition,
                LogContext{.module = "StateMachineController"},
                {Kv("detail", "turn_incremented"),
                 Kv("new_turn_id", static_cast<int64_t>(context.turn_id))});
      }
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_enter_waiting_command_window"),
               Kv("timeout_ms", 15000)});
      break;

    case ActionType::kEndSessionToWakeup:
      // End session and return to wakeup waiting
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_end_session_to_wakeup")});
      // Cancel command wait timeout if active
      controller.CancelTimeout(StateMachineController::TimeoutType::kCommandWait);
      // Clean up session state
      context.current_control_request_id.clear();
      context.nlu_control_state.has_pending_nlu_result = false;
      context.nlu_control_state.pending_nlu_result.reset();
      context.nlu_control_state.has_pending_asr_final_result = false;
      context.nlu_control_state.pending_asr_final_result.reset();
      context.nlu_control_state.pending_control_text.clear();
      context.nlu_control_state.wake_pos_samples.reset();
      // Reset ASR state
      context.asr_state.processed_samples = 0;
      context.asr_state.last_partial_text.clear();
      context.asr_state.has_pending_final_result = false;
      // Clear TTS tasks except maybe current one? Keep pending wake ACK?
      // For now, clear all tasks except any wake ACK that might be pending
      // (If we're ending session due to timeout, we shouldn't have pending wake ACK)
      if (!context.tts_state.wake_ack_pending) {
        context.tts_state.tasks.clear();
      }
      // Clear TTS started flag
      context.reply_tts_started = false;
      // Reset keep_session_open flag for new interaction
      context.keep_session_open = false;
      // Increment turn ID for new interaction
      context.turn_id++;
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "session_ended_to_wakeup"),
               Kv("new_turn_id", static_cast<int64_t>(context.turn_id))});
      break;

    case ActionType::kStartAsrForCommand:
      // Start ASR engine for command recognition
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_start_asr_for_command")});
      // Reset ASR engine and state
      if (context.config.asr.enabled && context.asr != nullptr) {
        Status st = context.asr->Reset();
        if (!st.ok()) {
          LogWarn(logevent::kAsrError,
                  LogContext{.module = "StateMachineController"},
                  {Kv("detail", "asr_reset_failed"),
                   Kv("err", st.message())});
        }
      }
      // Reset ASR state
      context.asr_state.processed_samples = 0;
      context.asr_state.last_partial_text.clear();
      context.asr_state.has_pending_final_result = false;
      context.asr_state.last_text_update_time = std::chrono::steady_clock::now();
      // Ensure ASR sub-state machine is in listening state
      context.subsm_state.asr = subsm::AsrState::kListening;
      // Reset keep_session_open flag for new command turn
      context.keep_session_open = false;
      // Cancel command wait timeout (speech detected, stop timeout)
      controller.CancelTimeout(StateMachineController::TimeoutType::kCommandWait);
      // Start ASR recognition timeout (15 seconds)
      controller.CancelTimeout(StateMachineController::TimeoutType::kAsrRecognition);
      controller.StartTimeout(StateMachineController::TimeoutType::kAsrRecognition, 8000);
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "asr_started_for_command")});
      break;

    case ActionType::kTriggerAsrFinalization:
      // Trigger ASR finalization on speech end
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_trigger_asr_finalization")});
      // Set flag to tell ASR stage to finalize
      context.asr_state.should_finalize = true;
      break;

    case ActionType::kSubmitAsrFinalToNlu:
      // Submit ASR final result to NLU
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_submit_asr_final_to_nlu"),
               Kv("text", event.text)});
      // Cancel ASR recognition timeout (ASR final result received)
      controller.CancelTimeout(StateMachineController::TimeoutType::kAsrRecognition);
      // TODO: Submit to NLU engine
      break;

    case ActionType::kCleanupKwsWindow:
      // Clean up KWS window state
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_cleanup_kws_window")});
      // TODO: Reset KWS state
      break;

    case ActionType::kCleanupAsrAndRearmCommandWait: {
      // Clean up ASR state and rearm command wait
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_cleanup_asr_rearm_command_wait")});
      // Reset ASR engine if enabled
      if (context.config.asr.enabled && context.asr != nullptr) {
        Status st = context.asr->Reset();
        if (!st.ok()) {
          LogWarn(logevent::kAsrError,
                  LogContext{.module = "StateMachineController"},
                  {Kv("detail", "asr_reset_failed_cleanup"),
                   Kv("err", st.message())});
        }
      }
      // Reset ASR state
      context.asr_state.processed_samples = 0;
      context.asr_state.last_partial_text.clear();
      context.asr_state.has_pending_final_result = false;
      context.asr_state.last_text_update_time = std::chrono::steady_clock::now();
      // Clear NLU control state pending ASR result
      context.nlu_control_state.has_pending_asr_final_result = false;
      context.nlu_control_state.pending_asr_final_result.reset();
      context.nlu_control_state.wake_pos_samples.reset();
      // Ensure ASR sub-state machine is in listening state
      context.subsm_state.asr = subsm::AsrState::kListening;
      // Cancel ASR recognition timeout (if any) and command wait timeout, then start fresh command wait
      controller.CancelTimeout(StateMachineController::TimeoutType::kAsrRecognition);
      controller.CancelTimeout(StateMachineController::TimeoutType::kCommandWait);
      controller.StartTimeout(StateMachineController::TimeoutType::kCommandWait, 15000);
      // ASR empty/timeout branches are ignored: just rearm command wait window.
      break;
    }

    case ActionType::kPrepareReplyAsrRetry:
      // Prepare retry reply "没听清，请再说一次。"
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_asr_retry")});
      // Schedule retry reply TTS task (same as ScheduleRetryReply)
      context.keep_session_open = true;
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", "没听清，请再说一次。"});
      break;

    case ActionType::kPrepareReplyUnknown:
      // Prepare unknown intent reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_unknown")});
      // Only treat as "didn't hear clearly" when NLU explicitly indicates
      // unknown intent with zero confidence and empty reply text.
      if (context.nlu_control_state.has_pending_nlu_result &&
          context.nlu_control_state.pending_nlu_result.has_value()) {
        const NluResult& nlu_result = *context.nlu_control_state.pending_nlu_result;
        if (nlu_result.intent == "unknown" &&
            nlu_result.confidence <= 0.0F &&
            nlu_result.reply_text.empty()) {
          context.keep_session_open = true;
          context.reply_tts_started = false;
          ++context.reply_playback_token;
          context.tts_state.tasks.push_back(TtsTask{"", "没听清，请再说一次。"});
        }
      }
      break;

    case ActionType::kSendControlRequest:
      // Send control request to device
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_send_control_request")});
      // Start synchronous control timeout (5 seconds)
      controller.StartTimeout(StateMachineController::TimeoutType::kControlSync, 5000);
      // Request ID will be set by control engine after execution
      // (context.current_control_request_id)
      LogDebug(logevent::kStateTransition,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "control_sync_timeout_started"),
                Kv("timeout_ms", 5000)});
      break;

    case ActionType::kPrepareReplyControlDone:
      // Prepare control completion reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_control_done")});
      // Cancel synchronous control timeout (request completed)
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlSync);
      // Also cancel asynchronous control timeout if active
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlAsync);
      // Queue TTS task with success text from event
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "control_done_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kPrepareReplyControlFail:
      // Prepare control failure reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_control_fail")});
      // Cancel synchronous control timeout (request completed)
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlSync);
      // Also cancel asynchronous control timeout if active
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlAsync);
      // Queue TTS task with failure text from event
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "control_fail_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kPrepareReplyControlTimeout:
      // Prepare control timeout reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_control_timeout")});
      // Cancel synchronous control timeout (request timed out)
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlSync);
      // Queue TTS task with timeout text from event
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "control_timeout_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kPrepareReplyControlTransportError:
      // Prepare control transport error reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_control_transport_error")});
      // Cancel synchronous control timeout (request failed)
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlSync);
      // Queue TTS task with transport error text from event
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "control_transport_error_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kStartWaitingAsyncNotify:
      // Start waiting for async control notification
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_start_waiting_async_notify")});
      // Cancel synchronous control timeout (async response received)
      controller.CancelTimeout(StateMachineController::TimeoutType::kControlSync);
      // Start asynchronous control timeout (e.g., 30 seconds)
      controller.StartTimeout(StateMachineController::TimeoutType::kControlAsync, 30000);
      LogDebug(logevent::kStateTransition,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "control_async_timeout_started"),
                Kv("timeout_ms", 30000)});
      break;

    case ActionType::kDiscardStaleControlAsyncEvent:
      // Discard stale async control event
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_discard_stale_control_async_event")});
      // No action needed
      break;

    case ActionType::kStartRagQuery:
      // Start RAG query
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_start_rag_query")});
      // Start RAG query timeout (default 5 seconds)
      controller.StartTimeout(StateMachineController::TimeoutType::kRagQuery, 5000);
      // TODO: Actually start RAG engine query
      break;

    case ActionType::kPrepareReplyQueryResult:
      // Prepare RAG query result reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_query_result")});
      // Cancel RAG query timeout
      controller.CancelTimeout(StateMachineController::TimeoutType::kRagQuery);
      // Queue TTS task with query result
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "rag_result_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kPrepareReplyQueryEmpty:
      // Prepare empty result reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_query_empty")});
      // Cancel RAG query timeout
      controller.CancelTimeout(StateMachineController::TimeoutType::kRagQuery);
      // Queue TTS task with empty result text
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text.empty() ? "未找到相关信息。" : event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "rag_empty_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kPrepareReplyQueryFail:
      // Prepare RAG query failure reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_query_fail")});
      // Cancel RAG query timeout
      controller.CancelTimeout(StateMachineController::TimeoutType::kRagQuery);
      // Queue TTS task with failure text
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text.empty() ? "查询失败，请稍后再试。" : event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "rag_fail_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kStartLlmChat:
      // Start LLM chat
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_start_llm_chat")});
      // Start LLM chat timeout (default 10 seconds)
      controller.StartTimeout(StateMachineController::TimeoutType::kLlmChat, 10000);
      // TODO: Actually start LLM engine chat
      break;

    case ActionType::kPrepareReplyLlmResult:
      // Prepare LLM chat result reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_llm_result")});
      // Cancel LLM chat timeout
      controller.CancelTimeout(StateMachineController::TimeoutType::kLlmChat);
      // Queue TTS task with LLM response
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "llm_result_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kPrepareReplyLlmFail:
      // Prepare LLM chat failure reply
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_prepare_reply_llm_fail")});
      // Cancel LLM chat timeout
      controller.CancelTimeout(StateMachineController::TimeoutType::kLlmChat);
      // Queue TTS task with failure text
      context.reply_tts_started = false;
      ++context.reply_playback_token;
      context.tts_state.tasks.push_back(TtsTask{"", event.text.empty() ? "对话失败，请稍后再试。" : event.text});
      LogDebug(logevent::kTtsStart,
               LogContext{.module = "StateMachineController"},
               {Kv("detail", "llm_fail_reply"),
                Kv("text", event.text.empty() ? "(empty)" : MaskSummary(event.text, 16))});
      break;

    case ActionType::kStopCurrentTtsAndStartAsr:
      // Stop current TTS and start ASR (barge-in)
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_stop_tts_start_asr")});
      // Clear pending TTS tasks
      context.tts_state.tasks.clear();
      // Clear TTS started flag
      context.reply_tts_started = false;
      // Reset ASR engine if enabled
      if (context.asr != nullptr && context.config.asr.enabled) {
        Status st = context.asr->Reset();
        if (!st.ok()) {
          LogWarn(logevent::kAsrError,
                  LogContext{.module = "StateMachineController"},
                  {Kv("detail", "asr_reset_failed_bargein"),
                   Kv("err", st.message())});
        }
      }
      break;

    case ActionType::kInitDone:
      // System initialization completed
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_init_done")});
      break;

    case ActionType::kRunErrorRecovery:
      // Initiate error recovery procedure
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_run_error_recovery")});
      // TODO: Start error recovery
      break;

    case ActionType::kEnterShuttingDown:
      // Begin system shutdown sequence
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_enter_shutting_down")});
      // TODO: Begin shutdown
      break;

    case ActionType::kRecordBootError:
      // Record boot failure error
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_record_boot_error")});
      // TODO: Record boot error
      break;

    case ActionType::kUpdateKeepSessionOpen:
      // Update keep_session_open flag
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_update_keep_session_open")});
      // TODO: Update based on intent
      break;

    case ActionType::kIncrementTurnId:
      // Increment turn ID
      context.turn_id++;
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_increment_turn_id"),
               Kv("new_turn_id", static_cast<int64_t>(context.turn_id))});
      break;

    case ActionType::kClearCurrentRequestId:
      // Clear current request ID
      context.current_control_request_id.clear();
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_clear_current_request_id")});
      break;

    case ActionType::kSetCurrentRequestId:
      // Set current request ID
      context.current_control_request_id = event.request_id;
      LogInfo(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "action_set_current_request_id"),
               Kv("request_id", event.request_id)});
      break;

    default:
      LogWarn(logevent::kStateTransition,
              LogContext{.module = "StateMachineController"},
              {Kv("detail", "unknown_action_type"),
               Kv("action", static_cast<int>(action))});
      break;
  }
}

}  // anonymous namespace

SessionState MapV3ToV2State(SessionState v3_state) {
  switch (v3_state) {
    // Direct mappings (same name, same meaning)
    case SessionState::kIdle:
    case SessionState::kWakeCandidate:
    case SessionState::kPreListening:
    case SessionState::kListening:
    case SessionState::kFinalizing:
    case SessionState::kRecognizing:
    case SessionState::kExecuting:
    case SessionState::kSpeaking:
      return v3_state;

    // v3-specific states map to closest v2 equivalent
    case SessionState::kBooting:
      return SessionState::kIdle;  // System not yet ready
    case SessionState::kAckSpeaking:
      return SessionState::kPreListening;  // Playing wake ACK
    case SessionState::kWaitingCommandSpeech:
      return SessionState::kPreListening;  // Waiting for command after wake
    case SessionState::kRecognizingCommand:
      return SessionState::kListening;     // ASR active
    case SessionState::kUnderstandingIntent:
      return SessionState::kRecognizing;   // NLU processing
    case SessionState::kExecutingControlSync:
      return SessionState::kExecuting;     // Control execution
    case SessionState::kWaitingControlAsync:
      return SessionState::kExecuting;     // Async control waiting
    case SessionState::kQueryingRag:
      return SessionState::kExecuting;     // RAG processing
    case SessionState::kChattingLlm:
      return SessionState::kExecuting;     // LLM processing
    case SessionState::kErrorRecovery:
      return SessionState::kIdle;          // Error recovery
    case SessionState::kShuttingDown:
      return SessionState::kIdle;          // Shutting down
    case SessionState::kAny:
      return SessionState::kIdle;          // Not a runtime state
    default:
      return SessionState::kIdle;          // Fallback
  }
}

// ============================================================================
// StateMachineController Implementation
// ============================================================================

StateMachineController::StateMachineController(SessionContext& context)
    : context_(context),
      current_state_(SessionState::kIdle) {
  // Initialize with idle state
  LogInfo(logevent::kStateTransition,
          LogContext{.module = "StateMachineController"},
          {Kv("detail", "controller_created"),
           Kv("initial_state", StateToString(current_state_))});

  // Sync initial state
  if (use_v3_state_machine_) {
    context_.state = MapV3ToV2State(current_state_);
  } else {
    context_.state = current_state_;
  }
}

void StateMachineController::ProcessEvent(const VisEvent& event) {
  std::lock_guard<std::mutex> lock(state_mutex_);

  SessionState from_state = current_state_;
  auto guard_eval = [&](GuardType guard) -> bool {
    return EvaluateGuardImpl(guard, event, context_);
  };

  using Transition =
      subsm::ExtendedTransition<SessionState, VisEventType, GuardType, ActionType>;
  static constexpr std::array<Transition, 44> kTransitionTable = {{
      // Boot / lifecycle
      {SessionState::kBooting, VisEventType::kBootCompleted, GuardType::kAlways,
       SessionState::kIdle, ActionType::kInitDone, 10, false},
      {SessionState::kBooting, VisEventType::kBootFailed, GuardType::kAlways,
       SessionState::kErrorRecovery, ActionType::kRecordBootError, 10, false},
      {SessionState::kErrorRecovery, VisEventType::kRecoverySucceeded, GuardType::kAlways,
       SessionState::kIdle, ActionType::kNoop, 10, false},
      {SessionState::kErrorRecovery, VisEventType::kRecoveryFailed, GuardType::kAlways,
       SessionState::kShuttingDown, ActionType::kEnterShuttingDown, 10, false},

      // Wake and command window
      {SessionState::kIdle, VisEventType::kKwsMatched, GuardType::kNotInWakeCooldown,
       SessionState::kAckSpeaking, ActionType::kPlayAck, 20, false},
      {SessionState::kIdle, VisEventType::kKwsMatched, GuardType::kInWakeCooldown,
       SessionState::kIdle, ActionType::kNoop, 10, false},
      {SessionState::kAckSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kAlways,
       SessionState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow, 10, false},
      {SessionState::kWaitingCommandSpeech, VisEventType::kCommandSpeechDetected, GuardType::kAlways,
       SessionState::kRecognizingCommand, ActionType::kStartAsrForCommand, 10, false},
      {SessionState::kWaitingCommandSpeech, VisEventType::kCommandWaitTimeout, GuardType::kAlways,
       SessionState::kIdle, ActionType::kEndSessionToWakeup, 10, false},
      {SessionState::kWaitingCommandSpeech, VisEventType::kKwsMatched, GuardType::kNotInWakeCooldown,
       SessionState::kAckSpeaking, ActionType::kPlayAck, 20, false},
      {SessionState::kWaitingCommandSpeech, VisEventType::kKwsMatched, GuardType::kInWakeCooldown,
       SessionState::kWaitingCommandSpeech, ActionType::kNoop, 10, false},

      // ASR / NLU
      {SessionState::kRecognizingCommand, VisEventType::kAsrFinalResult, GuardType::kAsrFinalTextNonEmpty,
       SessionState::kUnderstandingIntent, ActionType::kSubmitAsrFinalToNlu, 20, false},
      {SessionState::kRecognizingCommand, VisEventType::kAsrFinalResult, GuardType::kAsrFinalTextEmpty,
       SessionState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait, 10, false},
      {SessionState::kRecognizingCommand, VisEventType::kAsrEndpointWithEmptyText, GuardType::kAlways,
       SessionState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait, 10, false},
      {SessionState::kRecognizingCommand, VisEventType::kAsrTimeout, GuardType::kAlways,
       SessionState::kWaitingCommandSpeech, ActionType::kCleanupAsrAndRearmCommandWait, 10, false},
      {SessionState::kRecognizingCommand, VisEventType::kCommandSpeechEnd, GuardType::kAlways,
       SessionState::kRecognizingCommand, ActionType::kTriggerAsrFinalization, 10, false},
      {SessionState::kUnderstandingIntent, VisEventType::kIntentControl, GuardType::kControlEnabled,
       SessionState::kExecutingControlSync, ActionType::kSendControlRequest, 20, false},
      {SessionState::kUnderstandingIntent, VisEventType::kIntentRag, GuardType::kRagEnabled,
       SessionState::kQueryingRag, ActionType::kStartRagQuery, 20, false},
      {SessionState::kUnderstandingIntent, VisEventType::kIntentLlm, GuardType::kLlmEnabled,
       SessionState::kChattingLlm, ActionType::kStartLlmChat, 20, false},
      {SessionState::kUnderstandingIntent, VisEventType::kIntentUnknown, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyUnknown, 10, false},

      // Control flow
      {SessionState::kExecutingControlSync, VisEventType::kControlSyncAckSuccess, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyControlDone, 10, false},
      {SessionState::kExecutingControlSync, VisEventType::kControlSyncAckFail, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyControlFail, 10, false},
      {SessionState::kExecutingControlSync, VisEventType::kControlSyncAckTimeout, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyControlTimeout, 10, false},
      {SessionState::kExecutingControlSync, VisEventType::kControlTransportError, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyControlTransportError, 10, false},
      {SessionState::kExecutingControlSync, VisEventType::kControlAsyncSuccess, GuardType::kAlways,
       SessionState::kWaitingControlAsync, ActionType::kStartWaitingAsyncNotify, 10, false},

      {SessionState::kWaitingControlAsync, VisEventType::kControlAsyncSuccess,
       GuardType::kControlAsyncEventMatchesCurrent, SessionState::kSpeaking,
       ActionType::kPrepareReplyControlDone, 20, false},
      {SessionState::kWaitingControlAsync, VisEventType::kControlAsyncFail,
       GuardType::kControlAsyncEventMatchesCurrent, SessionState::kSpeaking,
       ActionType::kPrepareReplyControlFail, 20, false},
      {SessionState::kWaitingControlAsync, VisEventType::kControlAsyncTimeout, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyControlTimeout, 10, false},
      {SessionState::kWaitingControlAsync, VisEventType::kControlAsyncStaleOrMismatched,
       GuardType::kControlAsyncEventNotMatchesCurrent, SessionState::kWaitingControlAsync,
       ActionType::kDiscardStaleControlAsyncEvent, 20, false},

      // RAG flow
      {SessionState::kQueryingRag, VisEventType::kRagSuccess, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyQueryResult, 10, false},
      {SessionState::kQueryingRag, VisEventType::kRagEmpty, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyQueryEmpty, 10, false},
      {SessionState::kQueryingRag, VisEventType::kRagError, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyQueryFail, 10, false},
      {SessionState::kQueryingRag, VisEventType::kRagTimeout, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyQueryFail, 10, false},

      // LLM flow
      {SessionState::kChattingLlm, VisEventType::kLlmSuccess, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyLlmResult, 10, false},
      {SessionState::kChattingLlm, VisEventType::kLlmError, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyLlmFail, 10, false},
      {SessionState::kChattingLlm, VisEventType::kLlmTimeout, GuardType::kAlways,
       SessionState::kSpeaking, ActionType::kPrepareReplyLlmFail, 10, false},

      // TTS completion and barge-in
      {SessionState::kSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kKeepSessionOpen,
       SessionState::kWaitingCommandSpeech, ActionType::kEnterWaitingCommandWindow, 20, false},
      {SessionState::kSpeaking, VisEventType::kTtsPlaybackCompleted, GuardType::kNotKeepSessionOpen,
       SessionState::kIdle, ActionType::kEndSessionToWakeup, 10, false},
      {SessionState::kSpeaking, VisEventType::kUserBargeIn, GuardType::kBargeInEnabled,
       SessionState::kRecognizingCommand, ActionType::kStopCurrentTtsAndStartAsr, 20, false},

      // Global high-priority safety/lifecycle events
      {SessionState::kAny, VisEventType::kShutdownRequested, GuardType::kAlways,
       SessionState::kShuttingDown, ActionType::kEnterShuttingDown, 100, true},
      {SessionState::kAny, VisEventType::kFatalError, GuardType::kErrorRecoverable,
       SessionState::kErrorRecovery, ActionType::kRunErrorRecovery, 100, true},
      {SessionState::kAny, VisEventType::kFatalError, GuardType::kErrorNonRecoverable,
       SessionState::kShuttingDown, ActionType::kEnterShuttingDown, 90, true},
      {SessionState::kAny, VisEventType::kShutdownRequested, GuardType::kAlways,
       SessionState::kShuttingDown, ActionType::kNoop, 80, true},
      {SessionState::kAny, VisEventType::kTtsPlaybackFailed, GuardType::kAlways,
       SessionState::kIdle, ActionType::kEndSessionToWakeup, 50, true},
  }};
  const auto match = subsm::MatchTransitionWithPriority(
      kTransitionTable, current_state_, event.type, guard_eval);
  if (!match) {
    // No matching transition - log and ignore
    LogInfo(logevent::kStateTransition,
            LogContext{.module = "StateMachineController"},
            {Kv("detail", "no_matching_transition"),
             Kv("from_state", StateToString(current_state_)),
             Kv("event_type", EventTypeToString(event.type))});
    return;
  }

  // Execute transition
  SessionState to_state = match->to;
  ExecuteActionImpl(match->action, event, context_, *this);
  current_state_ = to_state;

  // Sync context state for backward compatibility
  if (use_v3_state_machine_) {
    // For now, map v3 state to v2 equivalent to keep pipeline stages working
    context_.state = MapV3ToV2State(current_state_);
  } else {
    // v2 mode: directly use v2 states (original 8)
    context_.state = current_state_;
  }

  // Log transition
  LogTransition(from_state, to_state, event, match->guard, match->action);
}

void StateMachineController::QueueEvent(const VisEvent& event) {
  std::lock_guard<std::mutex> lock(event_queue_mutex_);
  event_queue_.push_back(event);
}

void StateMachineController::Tick() {
  // Process queued events
  std::deque<VisEvent> events_to_process;
  {
    std::lock_guard<std::mutex> lock(event_queue_mutex_);
    events_to_process.swap(event_queue_);
  }

  for (const auto& event : events_to_process) {
    ProcessEvent(event);
  }

  // Check for expired timeouts
  CheckTimeouts();
}

SessionState StateMachineController::current_state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return current_state_;
}

std::string StateMachineController::current_state_name() const {
  return StateToString(current_state());
}

void StateMachineController::StartTimeout(TimeoutType type, int64_t duration_ms) {
  std::lock_guard<std::mutex> lock(timeouts_mutex_);
  auto expiry = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(duration_ms);
  active_timeouts_.push_back({type, expiry});

  LogInfo(logevent::kStateTransition,
          LogContext{.module = "StateMachineController"},
          {Kv("detail", "timeout_started"),
           Kv("type", static_cast<int>(type)),
           Kv("duration_ms", duration_ms)});
}

void StateMachineController::CancelTimeout(TimeoutType type) {
  std::lock_guard<std::mutex> lock(timeouts_mutex_);
  auto it = std::remove_if(active_timeouts_.begin(), active_timeouts_.end(),
                           [type](const TimeoutInfo& info) {
                             return info.type == type;
                           });
  active_timeouts_.erase(it, active_timeouts_.end());

  LogInfo(logevent::kStateTransition,
          LogContext{.module = "StateMachineController"},
          {Kv("detail", "timeout_cancelled"),
           Kv("type", static_cast<int>(type))});
}

void StateMachineController::CheckTimeouts() {
  std::lock_guard<std::mutex> lock(timeouts_mutex_);
  auto now = std::chrono::steady_clock::now();
  std::vector<TimeoutInfo> expired;

  auto it = std::remove_if(active_timeouts_.begin(), active_timeouts_.end(),
                           [now, &expired](const TimeoutInfo& info) {
                             if (info.expiry <= now) {
                               expired.push_back(info);
                               return true;
                             }
                             return false;
                           });
  active_timeouts_.erase(it, active_timeouts_.end());

  // Trigger timeout events
  for (const auto& timeout : expired) {
    VisEvent timeout_event(VisEventType::kCommandWaitTimeout);
    switch (timeout.type) {
      case TimeoutType::kCommandWait:
        timeout_event.type = VisEventType::kCommandWaitTimeout;
        break;
      case TimeoutType::kAsrRecognition:
        timeout_event.type = VisEventType::kAsrTimeout;
        break;
      case TimeoutType::kControlSync:
        timeout_event.type = VisEventType::kControlSyncAckTimeout;
        break;
      case TimeoutType::kControlAsync:
        timeout_event.type = VisEventType::kControlAsyncTimeout;
        break;
      case TimeoutType::kNluProcessing:
        timeout_event.type = VisEventType::kNluTimeout;
        break;
      case TimeoutType::kRagQuery:
        timeout_event.type = VisEventType::kRagTimeout;
        break;
      case TimeoutType::kLlmChat:
        timeout_event.type = VisEventType::kLlmTimeout;
        break;
    }
    QueueEvent(timeout_event);
  }
}

size_t StateMachineController::pending_event_count() const {
  std::lock_guard<std::mutex> lock(event_queue_mutex_);
  return event_queue_.size();
}

std::string StateMachineController::GetStatusString() const {
  std::lock_guard<std::mutex> lock1(state_mutex_);
  std::lock_guard<std::mutex> lock2(event_queue_mutex_);
  std::lock_guard<std::mutex> lock3(timeouts_mutex_);

  std::string status;
  status += "State: " + StateToString(current_state_) + "\n";
  status += "Pending events: " + std::to_string(event_queue_.size()) + "\n";
  status += "Active timeouts: " + std::to_string(active_timeouts_.size()) + "\n";
  for (const auto& timeout : active_timeouts_) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        timeout.expiry - std::chrono::steady_clock::now());
    status += "  - Type " + std::to_string(static_cast<int>(timeout.type)) +
              ", remaining " + std::to_string(remaining.count()) + "ms\n";
  }
  return status;
}

void StateMachineController::Reset() {
  std::lock_guard<std::mutex> lock1(state_mutex_);
  std::lock_guard<std::mutex> lock2(event_queue_mutex_);
  std::lock_guard<std::mutex> lock3(timeouts_mutex_);

  current_state_ = SessionState::kIdle;
  event_queue_.clear();
  active_timeouts_.clear();

  // Sync context state
  if (use_v3_state_machine_) {
    context_.state = MapV3ToV2State(current_state_);
  } else {
    context_.state = current_state_;
  }

  LogInfo(logevent::kStateTransition,
          LogContext{.module = "StateMachineController"},
          {Kv("detail", "controller_reset")});
}

bool StateMachineController::EvaluateGuard(GuardType guard, const VisEvent& event) {
  return EvaluateGuardImpl(guard, event, context_);
}

void StateMachineController::ExecuteAction(ActionType action, const VisEvent& event) {
  ExecuteActionImpl(action, event, context_, *this);
}

void StateMachineController::LogTransition(SessionState from_state,
                                           SessionState to_state,
                                           const VisEvent& event,
                                           GuardType guard,
                                           ActionType action) {
  LogInfo(logevent::kStateTransition,
          LogContext{.module = "StateMachineController"},
          {Kv("detail", "state_transition"),
           Kv("from", StateToString(from_state)),
           Kv("to", StateToString(to_state)),
           Kv("event", EventTypeToString(event.type)),
           Kv("guard", GuardTypeToString(guard)),
           Kv("action", ActionTypeToString(action)),
           Kv("event_text", event.text),
           Kv("request_id", event.request_id)});
}

VisEvent StateMachineController::ConvertSubsmEvent(const subsm::WakeDecision& decision) {
  // TODO: Implement conversion
  return VisEvent(VisEventType::kKwsMatched);
}

VisEvent StateMachineController::ConvertSubsmEvent(const subsm::AsrDecision& decision) {
  // TODO: Implement conversion
  return VisEvent(VisEventType::kAsrFinalResult);
}

VisEvent StateMachineController::ConvertSubsmEvent(const subsm::ReplyDecision& decision) {
  // TODO: Implement conversion
  return VisEvent(VisEventType::kTtsPlaybackCompleted);
}

}  // namespace mos::vis

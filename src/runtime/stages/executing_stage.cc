#include "mos/vis/runtime/stages/executing_stage.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "ExecutingStage";

LogContext MakeLogCtx(const SessionContext& context) {
  LogContext ctx;
  ctx.module = kLogTag;
  ctx.session = context.session_id;
  ctx.turn = context.turn_id;
  ctx.state = std::to_string(static_cast<int>(context.state));
  ctx.req = context.current_control_request_id;
  return ctx;
}

// Helper to trim ASCII whitespace
std::string TrimAsciiWhitespace(const std::string& s) {
  std::size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
    ++start;
  }
  std::size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return s.substr(start, end - start);
}

}  // namespace

ExecutingStage::ExecutingStage() = default;

ExecutingStage::~ExecutingStage() = default;

Status ExecutingStage::OnAttach(SessionContext& context) {
  // No special initialization needed
  return Status::Ok();
}

void ExecutingStage::OnDetach(SessionContext& context) {
  // No cleanup needed
}

bool ExecutingStage::CanProcess(SessionState state) const {
  return state == SessionState::kExecuting;
}

Status ExecutingStage::Process(SessionContext& context) {
  auto& nlu_control = context.nlu_control_state;
  if (!nlu_control.has_pending_nlu_result) {
    // No NLU result pending; transition to idle
    context.state = SessionState::kIdle;
    LogInfo(logevent::kSessionEnd, MakeLogCtx(context), {Kv("reason", "no_pending_nlu_result")});
    return Status::Ok();
  }

  const NluResult& nlu_result = *nlu_control.pending_nlu_result;
  const std::string intent = nlu_result.intent;
  const bool is_control_intent = intent.rfind("device.control.", 0) == 0;
  const bool is_unknown_intent = intent == "unknown";
  context.keep_session_open = true;

  std::string reply_text;
  if (!is_control_intent && !nlu_result.reply_text.empty()) {
    reply_text = nlu_result.reply_text;
  }

  ControlRequest control_request;
  control_request.intent = intent;
  control_request.confidence = nlu_result.confidence;
  control_request.text = nlu_control.pending_control_text;
  control_request.nlu_json = nlu_result.json;
  control_request.session_id = context.session_id;
  control_request.turn_id = context.turn_id;

  // Execute control command if applicable
  const bool should_execute_control =
      is_control_intent && !intent.empty() && intent != "unknown";
  if (should_execute_control && context.control != nullptr) {
    Status st = ExecuteControl(context, control_request, reply_text);
    if (!st.ok()) {
      // Error already logged by ExecuteControl
      // reply_text may have been set to error message
    }
  } else if (nlu_control.has_pending_nlu_result) {
    LogInfo(logevent::kIntentRoute, MakeLogCtx(context),
            {Kv("detail", "control_execute_skipped"), Kv("intent", intent)});
  }

  // Schedule TTS reply if we have one
  if (!reply_text.empty()) {
    if (intent == "device.control.stop_analysis") {
      context.keep_session_open = false;
    }
    context.reply_tts_started = false;
    ++context.reply_playback_token;
    context.tts_state.tasks.push_back(TtsTask{"", reply_text});
    nlu_control.has_pending_nlu_result = false;
    nlu_control.pending_nlu_result.reset();
    nlu_control.pending_control_text.clear();
    // Remain in executing state? The original returns early.
    // The pipeline scheduler will call TtsStage later.
    return Status::Ok();
  }

  // No reply text
  if (is_unknown_intent) {
    nlu_control.has_pending_nlu_result = false;
    nlu_control.pending_nlu_result.reset();
    nlu_control.pending_control_text.clear();
    context.state = SessionState::kPreListening;
    LogInfo(logevent::kStateTransition, MakeLogCtx(context),
            {Kv("layer", "main"), Kv("from", "executing"), Kv("to", "pre_listening"), Kv("reason", "unknown_intent")});
    return Status::Ok();
  }

  // Known intent but no reply text and no control execution?
  // Transition to idle (original code sets kThinking then kIdle)
  nlu_control.has_pending_nlu_result = false;
  nlu_control.pending_nlu_result.reset();
  nlu_control.pending_control_text.clear();
  context.state = SessionState::kIdle;
  LogInfo(logevent::kSessionEnd, MakeLogCtx(context),
          {Kv("reason", "known_intent_no_reply")});
  return Status::Ok();
}

Status ExecutingStage::ExecuteControl(SessionContext& context,
                                      const ControlRequest& request,
                                      std::string& reply_text) {
  const ScopedTimer timer;
  LogInfo(logevent::kControlSend, MakeLogCtx(context),
          {Kv("intent", request.intent), Kv("text", MaskSummary(request.text, 20))});
  Status st = context.control->Reset();
  if (!st.ok()) {
    LogWarn(logevent::kControlRetry, MakeLogCtx(context),
            {Kv("detail", "control_reset_failed"), Kv("err", st.message())});
  }
  ControlResult control_result;
  st = context.control->Execute(request, &control_result);
  if (!st.ok()) {
    LogError(logevent::kControlTimeout, MakeLogCtx(context),
             {Kv("detail", "control_execute_failed"), Kv("err", st.message())});
    reply_text = "控制请求失败：" + st.message();
    return st;
  }
  LogInfo(logevent::kControlAck, MakeLogCtx(context),
          {Kv("handled", control_result.handled ? 1 : 0),
           Kv("action_name", control_result.action),
           Kv("ack_ms", timer.ElapsedMs())});
  context.current_control_request_id = control_result.request_id;
  reply_text = control_result.reply_text;
  // If intent was known but not handled, treat as unknown (handled by caller)
  // This logic is in the original but we rely on caller's is_unknown_intent flag.
  return Status::Ok();
}

}  // namespace mos::vis

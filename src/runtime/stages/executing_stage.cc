#include "mos/vis/runtime/stages/executing_stage.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "ExecutingStage";

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
    GetLogger()->info("{}: wait for awake", kLogTag);
    return Status::Ok();
  }

  const NluResult& nlu_result = *nlu_control.pending_nlu_result;
  const std::string intent = nlu_result.intent;
  const bool is_control_intent = intent.rfind("device.control.", 0) == 0;
  const bool is_unknown_intent = intent == "unknown";

  std::string reply_text;
  if (!is_control_intent && !nlu_result.reply_text.empty()) {
    reply_text = nlu_result.reply_text;
  }

  ControlRequest control_request;
  control_request.intent = intent;
  control_request.confidence = nlu_result.confidence;
  control_request.text = nlu_control.pending_control_text;
  control_request.nlu_json = nlu_result.json;

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
    GetLogger()->info("{}: control execute skipped: intent={}", kLogTag, intent);
  }

  // Schedule TTS reply if we have one
  if (!reply_text.empty()) {
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
    GetLogger()->info("{}: wait for instruction", kLogTag);
    return Status::Ok();
  }

  // Known intent but no reply text and no control execution?
  // Transition to idle (original code sets kThinking then kIdle)
  nlu_control.has_pending_nlu_result = false;
  nlu_control.pending_nlu_result.reset();
  nlu_control.pending_control_text.clear();
  context.state = SessionState::kIdle;
  GetLogger()->info("{}: wait for awake", kLogTag);
  return Status::Ok();
}

Status ExecutingStage::ExecuteControl(SessionContext& context,
                                      const ControlRequest& request,
                                      std::string& reply_text) {
  GetLogger()->info("{}: control execute begin: intent={} text={}",
                    kLogTag, request.intent, request.text);
  Status st = context.control->Reset();
  if (!st.ok()) {
    GetLogger()->warn("{}: control reset failed: {}", kLogTag, st.message());
  }
  ControlResult control_result;
  st = context.control->Execute(request, &control_result);
  if (!st.ok()) {
    GetLogger()->warn("{}: control execute failed: {}", kLogTag, st.message());
    reply_text = "控制请求失败：" + st.message();
    return st;
  }
  GetLogger()->info("{}: control execute done: handled={} action={} reply={}",
                    kLogTag,
                    control_result.handled ? 1 : 0,
                    control_result.action,
                    control_result.reply_text);
  reply_text = control_result.reply_text;
  // If intent was known but not handled, treat as unknown (handled by caller)
  // This logic is in the original but we rely on caller's is_unknown_intent flag.
  return Status::Ok();
}

}  // namespace mos::vis
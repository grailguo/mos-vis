#include "mos/vis/runtime/stages/recognizing_stage.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "mos/vis/common/logging.h"
#include "mos/vis/runtime/vis_event.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "RecognizingStage";

LogContext MakeLogCtx(const SessionContext& context) {
  LogContext ctx;
  ctx.module = kLogTag;
  ctx.session = context.session_id;
  ctx.turn = context.turn_id;
  ctx.state = std::to_string(static_cast<int>(context.state));
  ctx.req = context.current_control_request_id;
  return ctx;
}

}  // namespace

RecognizingStage::RecognizingStage() = default;

RecognizingStage::~RecognizingStage() = default;

Status RecognizingStage::OnAttach(SessionContext& context) {
  // No special initialization needed
  return Status::Ok();
}

void RecognizingStage::OnDetach(SessionContext& context) {
  // No cleanup needed
}

bool RecognizingStage::CanProcess(SessionState state) const {
  return state == SessionState::kRecognizing;
}

Status RecognizingStage::Process(SessionContext& context) {
  auto& nlu_control = context.nlu_control_state;
  if (!nlu_control.has_pending_asr_final_result || !nlu_control.pending_asr_final_result) {
    LogWarn(logevent::kAsrError, MakeLogCtx(context),
            {Kv("detail", "entered_without_pending_asr_result")});
    context.state = SessionState::kIdle;
    LogInfo(logevent::kSessionEnd, MakeLogCtx(context), {Kv("reason", "recognizing_without_input")});
    return Status::Ok();
  }

  const AsrResult& final_result = *nlu_control.pending_asr_final_result;
  const std::string final_text = TrimAsciiWhitespace(final_result.text);
  if (final_text.empty()) {
    LogWarn(logevent::kAsrFinalEmpty, MakeLogCtx(context), {Kv("detail", "skip_recognizing")});
    nlu_control.has_pending_asr_final_result = false;
    nlu_control.pending_asr_final_result.reset();
    // Queue empty text event for state machine
    if (context.state_machine) {
      VisEvent event(VisEventType::kAsrEndpointWithEmptyText);
      event.source_stage = "RecognizingStage";
      context.state_machine->QueueEvent(event);
    }
    return Status::Ok();
  }

  LogInfo(logevent::kAsrFinal, MakeLogCtx(context), {Kv("text", MaskSummary(final_text, 16))});

  // Prepare NLU inference
  nlu_control.pending_nlu_result.reset();
  nlu_control.has_pending_nlu_result = false;
  nlu_control.pending_control_text = final_text;

  VisEventType intent_event_type = VisEventType::kIntentUnknown;
  if (context.nlu != nullptr) {
    Status st = context.nlu->Reset();
    if (!st.ok()) {
      GetLogger()->warn("{}: NLU reset failed: {}", kLogTag, st.message());
      LogWarn(logevent::kNluError, MakeLogCtx(context), {Kv("detail", "nlu_reset_failed"), Kv("err", st.message())});
    }
    NluResult nlu_result;
    st = context.nlu->Infer(final_result.text, &nlu_result);
    if (!st.ok()) {
      LogWarn(logevent::kNluError, MakeLogCtx(context), {Kv("detail", "nlu_infer_failed"), Kv("err", st.message())});
    } else {
      LogInfo(logevent::kIntentRoute, MakeLogCtx(context),
              {Kv("intent", nlu_result.intent), Kv("confidence", nlu_result.confidence)});
      if (nlu_result.intent.rfind("device.control.", 0) == 0) {
        LogInfo(logevent::kIntentRoute, MakeLogCtx(context),
                {Kv("detail", "matched_control_intent"), Kv("intent", nlu_result.intent)});
        intent_event_type = VisEventType::kIntentControl;
      } else if (nlu_result.intent == "unknown") {
        intent_event_type = VisEventType::kIntentUnknown;
      } else {
        // Default to unknown for any other intent
        intent_event_type = VisEventType::kIntentUnknown;
      }
      nlu_control.pending_nlu_result = std::move(nlu_result);
      nlu_control.has_pending_nlu_result = true;
    }
  }

  // Queue intent event for state machine
  if (context.state_machine) {
    VisEvent event(intent_event_type);
    event.source_stage = "RecognizingStage";
    context.state_machine->QueueEvent(event);
  }

  // Clear pending ASR result (state machine will transition)
  nlu_control.has_pending_asr_final_result = false;
  nlu_control.pending_asr_final_result.reset();

  return Status::Ok();
}

std::string RecognizingStage::TrimAsciiWhitespace(const std::string& s) {
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

}  // namespace mos::vis

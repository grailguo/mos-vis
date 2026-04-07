#include "mos/vis/runtime/stages/recognizing_stage.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "RecognizingStage";

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
    GetLogger()->warn("{}: entered without pending ASR result", kLogTag);
    context.state = SessionState::kIdle;
    GetLogger()->info("{}: wait for awake", kLogTag);
    return Status::Ok();
  }

  const AsrResult& final_result = *nlu_control.pending_asr_final_result;
  const std::string final_text = TrimAsciiWhitespace(final_result.text);
  if (final_text.empty()) {
    GetLogger()->warn("{}: final ASR text is empty, skip recognizing", kLogTag);
    nlu_control.has_pending_asr_final_result = false;
    nlu_control.pending_asr_final_result.reset();
    context.state = SessionState::kPreListening;
    GetLogger()->info("{}: wait for instruction", kLogTag);
    return Status::Ok();
  }

  GetLogger()->info("{}: ASR final text: {}", kLogTag, final_text);

  // Prepare NLU inference
  nlu_control.pending_nlu_result.reset();
  nlu_control.has_pending_nlu_result = false;
  nlu_control.pending_control_text = final_text;

  if (context.nlu != nullptr) {
    Status st = context.nlu->Reset();
    if (!st.ok()) {
      GetLogger()->warn("{}: NLU reset failed: {}", kLogTag, st.message());
    }
    NluResult nlu_result;
    st = context.nlu->Infer(final_result.text, &nlu_result);
    if (!st.ok()) {
      GetLogger()->warn("{}: NLU infer failed: {}", kLogTag, st.message());
    } else {
      GetLogger()->info("{}: NLU result: intent={} confidence={:.3f}",
                        kLogTag, nlu_result.intent, nlu_result.confidence);
      if (nlu_result.intent.rfind("device.control.", 0) == 0) {
        GetLogger()->info("{}: NLU matched control command: {}", kLogTag, nlu_result.intent);
      }
      nlu_control.pending_nlu_result = std::move(nlu_result);
      nlu_control.has_pending_nlu_result = true;
    }
  }

  // Transition to executing state
  context.state = SessionState::kExecuting;
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
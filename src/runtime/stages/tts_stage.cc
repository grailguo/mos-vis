#include "mos/vis/runtime/stages/tts_stage.h"

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "TtsStage";

}  // namespace

TtsStage::TtsStage() = default;

TtsStage::~TtsStage() = default;

Status TtsStage::OnAttach(SessionContext& context) {
  // No special initialization needed
  return Status::Ok();
}

void TtsStage::OnDetach(SessionContext& context) {
  // No cleanup needed
}

bool TtsStage::CanProcess(SessionState state) const {
  // TTS stage can process in any state; it will only run if there are pending tasks
  return true;
}

Status TtsStage::Process(SessionContext& context) {
  if (!context.config.tts.enabled || context.tts == nullptr) {
    return Status::Ok();
  }

  auto& tts_state = context.tts_state;
  if (tts_state.task_running || tts_state.tasks.empty()) {
    return Status::Ok();
  }

  // Take the next task
  const TtsTask task = tts_state.tasks.front();
  tts_state.tasks.pop_front();
  tts_state.task_running = true;
  context.state = SessionState::kSpeaking;

  Status st = StartTask(context, task);
  if (!st.ok()) {
    GetLogger()->warn("{}: TTS task failed: {}", kLogTag, st.message());
  }

  tts_state.wake_ack_pending = false;
  tts_state.task_running = false;

  // Determine next state
  if (context.nlu_control_state.has_pending_nlu_result) {
    // This case seems to be a fallback in the original code
    context.nlu_control_state.has_pending_nlu_result = false;
    context.nlu_control_state.pending_nlu_result.reset();
    context.nlu_control_state.pending_control_text.clear();
    context.state = SessionState::kIdle;
    GetLogger()->info("{}: wait for awake", kLogTag);
  } else {
    context.state = SessionState::kPreListening;
    GetLogger()->info("{}: wait for instruction", kLogTag);
  }

  return Status::Ok();
}

Status TtsStage::StartTask(SessionContext& context, const TtsTask& task) {
  if (!task.preset_file.empty()) {
    GetLogger()->debug("{}: playing preset file: {}", kLogTag, task.preset_file);
    return context.tts->PlayFile(task.preset_file);
  } else if (!task.reply_text.empty()) {
    GetLogger()->debug("{}: speaking text: {}", kLogTag, task.reply_text);
    return context.tts->Speak(task.reply_text);
  }
  // Empty task – nothing to do
  return Status::Ok();
}

}  // namespace mos::vis
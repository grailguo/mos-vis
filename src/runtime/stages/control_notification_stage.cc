#include "mos/vis/runtime/stages/control_notification_stage.h"

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "ControlNotificationStage";

}  // namespace

ControlNotificationStage::ControlNotificationStage() = default;

ControlNotificationStage::~ControlNotificationStage() = default;

Status ControlNotificationStage::OnAttach(SessionContext& context) {
  // No special initialization needed
  return Status::Ok();
}

void ControlNotificationStage::OnDetach(SessionContext& context) {
  // No cleanup needed
}

bool ControlNotificationStage::CanProcess(SessionState state) const {
  // Notification stage is always active (polling for notifications)
  return true;
}

Status ControlNotificationStage::Process(SessionContext& context) {
  if (context.control == nullptr) {
    return Status::Ok();
  }

  ControlResult notify_result;
  Status st = context.control->PollNotification(&notify_result);
  if (!st.ok()) {
    GetLogger()->warn("{}: control notification poll failed: {}", kLogTag, st.message());
    return Status::Ok();
  }
  if (!notify_result.reply_text.empty()) {
    GetLogger()->info("{}: control notify: action={} reply={}",
                      kLogTag, notify_result.action, notify_result.reply_text);
    context.tts_state.tasks.push_back(TtsTask{"", notify_result.reply_text});
  }
  return Status::Ok();
}

}  // namespace mos::vis
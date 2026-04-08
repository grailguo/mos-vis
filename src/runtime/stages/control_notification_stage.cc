#include "mos/vis/runtime/stages/control_notification_stage.h"

#include "mos/vis/common/logging.h"
#include <nlohmann/json.hpp>

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "ControlNotificationStage";

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
    LogWarn(logevent::kControlTransportDisconnect, MakeLogCtx(context),
            {Kv("detail", "poll_failed"), Kv("err", st.message())});
    return Status::Ok();
  }
  if (!notify_result.reply_text.empty()) {
    std::string notify_request_id = notify_result.request_id;
    if (notify_request_id.empty() && !notify_result.ws_payload_json.empty()) {
      try {
        const auto j = nlohmann::json::parse(notify_result.ws_payload_json);
        if (j.contains("result") && j.at("result").is_object()) {
          const auto& r = j.at("result");
          if (r.contains("analysis_uuid") && r.at("analysis_uuid").is_string()) {
            notify_request_id = r.at("analysis_uuid").get<std::string>();
          } else if (r.contains("task_id") && r.at("task_id").is_string()) {
            notify_request_id = r.at("task_id").get<std::string>();
          }
        }
      } catch (const std::exception&) {
      }
    }

    if (!context.current_control_request_id.empty() &&
        !notify_request_id.empty() &&
        notify_request_id != context.current_control_request_id) {
      LogWarn(logevent::kControlStaleNotifyDrop, MakeLogCtx(context),
              {Kv("action_name", notify_result.action),
               Kv("notify_req", notify_request_id),
               Kv("expected_req", context.current_control_request_id)});
      return Status::Ok();
    }

    LogInfo(logevent::kControlNotify, MakeLogCtx(context),
            {Kv("action_name", notify_result.action),
             Kv("req", notify_request_id.empty() ? context.current_control_request_id : notify_request_id),
             Kv("reply", MaskSummary(notify_result.reply_text, 24))});
    context.keep_session_open = true;
    context.reply_tts_started = false;
    ++context.reply_playback_token;
    context.tts_state.tasks.push_back(TtsTask{"", notify_result.reply_text});
  }
  return Status::Ok();
}

}  // namespace mos::vis

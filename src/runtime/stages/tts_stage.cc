#include "mos/vis/runtime/stages/tts_stage.h"

#include "mos/vis/common/logging.h"
#include "mos/vis/runtime/subsm/hotspot_subsms.h"
#include "mos/vis/runtime/vis_event.h"
#include "mos/vis/runtime/session_state.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "TtsStage";

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
  ConsumeReplyEvents(context);

  if (!context.config.tts.enabled || context.tts == nullptr) {
    return Status::Ok();
  }

  auto& tts_state = context.tts_state;
  if (tts_state.task_running || tts_state.tasks.empty()) {
    return Status::Ok();
  }

  // Take the next task
  const ScopedTimer timer;
  const TtsTask task = tts_state.tasks.front();
  tts_state.tasks.pop_front();
  tts_state.task_running = true;
  context.reply_tts_started = true;
  ++context.reply_playback_token;
  // State should already be kSpeaking (for reply) or kAckSpeaking (for wake ack)
  // via state machine transitions; do not assign directly.

  Status st = StartTask(context, task);
  if (!st.ok()) {
    LogWarn(logevent::kTtsFail, MakeLogCtx(context), {Kv("err", st.message())});
    context.local_events.reply_events.push_back(subsm::ReplyEvent::kTtsPlaybackFailed);
  } else {
    LogInfo(logevent::kTtsDone, MakeLogCtx(context), {Kv("cost_ms", timer.ElapsedMs())});
    context.local_events.reply_events.push_back(subsm::ReplyEvent::kTtsPlaybackCompleted);
  }

  tts_state.wake_ack_pending = false;
  tts_state.task_running = false;
  ConsumeReplyEvents(context);

  return Status::Ok();
}

Status TtsStage::StartTask(SessionContext& context, const TtsTask& task) {
  if (!task.preset_file.empty()) {
    LogDebug(logevent::kTtsStart, MakeLogCtx(context), {Kv("preset", BasenamePath(task.preset_file))});
    return context.tts->PlayFile(task.preset_file);
  } else if (!task.reply_text.empty()) {
    LogDebug(logevent::kTtsStart, MakeLogCtx(context), {Kv("text", MaskSummary(task.reply_text, 16))});
    return context.tts->Speak(task.reply_text);
  }
  // Empty task – nothing to do
  return Status::Ok();
}

void TtsStage::ConsumeReplyEvents(SessionContext& context) {
  auto& events = context.local_events.reply_events;
  while (!events.empty()) {
    const subsm::ReplyEvent event = events.front();
    events.pop_front();

    const subsm::ReplyDecision decision =
        subsm::StepReply(context.subsm_state.reply,
                         event,
                         context.keep_session_open,
                         context.barge_in_enabled);

    LogDebug(logevent::kSubsmTransition, MakeLogCtx(context),
             {Kv("subsm", "reply"),
              Kv("from", static_cast<int>(context.subsm_state.reply)),
              Kv("ev", static_cast<int>(event)),
              Kv("guard", context.keep_session_open ? "keep_open" : "close_session"),
              Kv("to", static_cast<int>(decision.next_state)),
              Kv("action", static_cast<int>(decision.action))});

    context.subsm_state.reply = decision.next_state;

    // Map sub-state machine event to VisEvent for main state machine
    VisEventType vis_type = VisEventType::kTtsPlaybackCompleted; // default
    switch (event) {
      case subsm::ReplyEvent::kTtsPlaybackCompleted:
        vis_type = VisEventType::kTtsPlaybackCompleted;
        break;
      case subsm::ReplyEvent::kTtsPlaybackFailed:
        vis_type = VisEventType::kTtsPlaybackFailed;
        break;
      case subsm::ReplyEvent::kUserBargeIn:
        vis_type = VisEventType::kUserBargeIn;
        break;
      default:
        // Unknown event, ignore
        continue;
    }

    // Queue event to main state machine if available
    if (context.state_machine) {
      VisEvent vis_event(vis_type);
      vis_event.source_stage = "TtsStage";
      context.state_machine->QueueEvent(vis_event);
      // Log that we queued event
      LogDebug(logevent::kSubsmTransition, MakeLogCtx(context),
               {Kv("detail", "queued_vis_event"),
                Kv("vis_type", static_cast<int>(vis_type))});
    } else {
      // Fallback to original v2 state assignments
      switch (decision.action) {
        case subsm::ReplyAction::kRearmCommandWait:
          context.state = SessionState::kPreListening;
          context.reply_tts_started = false;
          ++context.turn_id;
          LogInfo(logevent::kStateTransition, MakeLogCtx(context),
                  {Kv("layer", "main"),
                   Kv("from", "speaking"),
                   Kv("to", "pre_listening"),
                   Kv("reason", "tts_done_keep_session")});
          break;
        case subsm::ReplyAction::kEndSessionToIdle:
          context.state = SessionState::kIdle;
          context.reply_tts_started = false;
          LogInfo(logevent::kSessionEnd, MakeLogCtx(context),
                  {Kv("reason", "tts_done_close_session")});
          break;
        case subsm::ReplyAction::kStartAsrByBargeIn:
          // Current TTS engine has no hard-stop API; switch to listening path and clear queued reply.
          context.tts_state.tasks.clear();
          context.state = SessionState::kListening;
          context.reply_tts_started = false;
          if (context.asr != nullptr && context.config.asr.enabled) {
            Status st = context.asr->Reset();
            if (!st.ok()) {
              GetLogger()->warn("{}: ASR reset failed on barge-in: {}", kLogTag, st.message());
            }
          }
          LogWarn(logevent::kTtsBargeIn, MakeLogCtx(context), {Kv("detail", "switch_to_listening")});
          break;
        case subsm::ReplyAction::kNoop:
        default:
          break;
      }
    }
  }
}

}  // namespace mos::vis

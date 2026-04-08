#include "mos/vis/runtime/subsm/hotspot_subsms.h"

#include <array>

#include "mos/vis/runtime/subsm/transition_map.h"

namespace mos::vis::subsm {

WakeDecision StepWake(WakeState current, WakeEvent event, bool in_cooldown) {
  using Row = Transition<WakeState, WakeEvent, WakeGuard, WakeAction>;
  static constexpr std::array<Row, 4> kTable{{
      {WakeState::kWaitingWakeup, WakeEvent::kKwsMatched, WakeGuard::kNotInCooldown,
       WakeState::kWakeDetecting, WakeAction::kFireWake},
      {WakeState::kWaitingWakeup, WakeEvent::kKwsMatched, WakeGuard::kInCooldown,
       WakeState::kWaitingWakeup, WakeAction::kIgnoreMatched},
      {WakeState::kWakeDetecting, WakeEvent::kKwsRejected, WakeGuard::kAlways,
       WakeState::kWaitingWakeup, WakeAction::kCleanupWindow},
      {WakeState::kWakeDetecting, WakeEvent::kKwsTimeout, WakeGuard::kAlways,
       WakeState::kWaitingWakeup, WakeAction::kCleanupWindow},
  }};

  auto guard_eval = [in_cooldown](WakeGuard guard) {
    switch (guard) {
      case WakeGuard::kAlways:
        return true;
      case WakeGuard::kInCooldown:
        return in_cooldown;
      case WakeGuard::kNotInCooldown:
        return !in_cooldown;
    }
    return false;
  };

  const auto row = MatchTransition(kTable, current, event, guard_eval);
  if (!row.has_value()) {
    return {current, WakeAction::kNoop};
  }
  return {row->to, row->action};
}

AsrDecision StepAsr(AsrState current, AsrEvent event) {
  using Row = Transition<AsrState, AsrEvent, AsrGuard, AsrAction>;
  static constexpr std::array<Row, 4> kTable{{
      {AsrState::kListening, AsrEvent::kAsrFinalNonEmpty, AsrGuard::kAlways,
       AsrState::kRecognizing, AsrAction::kToRecognizing},
      {AsrState::kFinalizing, AsrEvent::kAsrFinalNonEmpty, AsrGuard::kAlways,
       AsrState::kRecognizing, AsrAction::kToRecognizing},
      {AsrState::kListening, AsrEvent::kAsrFinalEmpty, AsrGuard::kAlways,
       AsrState::kListening, AsrAction::kPrepareRetryReply},
      {AsrState::kListening, AsrEvent::kAsrTimeout, AsrGuard::kAlways,
       AsrState::kListening, AsrAction::kPrepareRetryReply},
  }};

  if (event == AsrEvent::kAsrError) {
    return {current, AsrAction::kPrepareRetryReply};
  }

  const auto row = MatchTransition(
      kTable, current, event, [](AsrGuard guard) { return guard == AsrGuard::kAlways; });
  if (!row.has_value()) {
    return {current, AsrAction::kNoop};
  }
  return {row->to, row->action};
}

ReplyDecision StepReply(ReplyState current,
                        ReplyEvent event,
                        bool keep_session_open,
                        bool barge_in_enabled) {
  using Row = Transition<ReplyState, ReplyEvent, ReplyGuard, ReplyAction>;
  static constexpr std::array<Row, 5> kTable{{
      {ReplyState::kResultSpeaking, ReplyEvent::kTtsPlaybackCompleted, ReplyGuard::kKeepSessionOpen,
       ReplyState::kResultSpeaking, ReplyAction::kRearmCommandWait},
      {ReplyState::kResultSpeaking, ReplyEvent::kTtsPlaybackCompleted, ReplyGuard::kNotKeepSessionOpen,
       ReplyState::kResultSpeaking, ReplyAction::kEndSessionToIdle},
      {ReplyState::kResultSpeaking, ReplyEvent::kTtsPlaybackFailed, ReplyGuard::kKeepSessionOpen,
       ReplyState::kResultSpeaking, ReplyAction::kRearmCommandWait},
      {ReplyState::kResultSpeaking, ReplyEvent::kTtsPlaybackFailed, ReplyGuard::kNotKeepSessionOpen,
       ReplyState::kResultSpeaking, ReplyAction::kEndSessionToIdle},
      {ReplyState::kResultSpeaking, ReplyEvent::kUserBargeIn, ReplyGuard::kBargeInEnabled,
       ReplyState::kResultSpeaking, ReplyAction::kStartAsrByBargeIn},
  }};

  auto guard_eval = [keep_session_open, barge_in_enabled](ReplyGuard guard) {
    switch (guard) {
      case ReplyGuard::kAlways:
        return true;
      case ReplyGuard::kKeepSessionOpen:
        return keep_session_open;
      case ReplyGuard::kNotKeepSessionOpen:
        return !keep_session_open;
      case ReplyGuard::kBargeInEnabled:
        return barge_in_enabled;
    }
    return false;
  };

  const auto row = MatchTransition(kTable, current, event, guard_eval);
  if (!row.has_value()) {
    return {current, ReplyAction::kNoop};
  }
  return {row->to, row->action};
}

}  // namespace mos::vis::subsm


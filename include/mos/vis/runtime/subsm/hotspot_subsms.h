#pragma once

#include <deque>
#include <string>

#include "mos/vis/runtime/session_state.h"

namespace mos::vis::subsm {

enum class WakeState {
  kWaitingWakeup = 0,
  kWakeDetecting,
};

enum class WakeEvent {
  kKwsMatched = 0,
  kKwsRejected,
  kKwsTimeout,
};

enum class WakeGuard {
  kAlways = 0,
  kInCooldown,
  kNotInCooldown,
};

enum class WakeAction {
  kNoop = 0,
  kFireWake,
  kIgnoreMatched,
  kCleanupWindow,
};

struct WakeDecision {
  WakeState next_state = WakeState::kWaitingWakeup;
  WakeAction action = WakeAction::kNoop;
};

WakeDecision StepWake(WakeState current, WakeEvent event, bool in_cooldown);

enum class AsrState {
  kListening = 0,
  kFinalizing,
  kRecognizing,
};

enum class AsrEvent {
  kAsrPartial = 0,
  kAsrFinalNonEmpty,
  kAsrFinalEmpty,
  kAsrTimeout,
  kAsrError,
};

enum class AsrGuard {
  kAlways = 0,
};

enum class AsrAction {
  kNoop = 0,
  kToRecognizing,
  kPrepareRetryReply,
};

struct AsrDecision {
  AsrState next_state = AsrState::kListening;
  AsrAction action = AsrAction::kNoop;
};

AsrDecision StepAsr(AsrState current, AsrEvent event);

enum class ReplyState {
  kResultSpeaking = 0,
};

enum class ReplyEvent {
  kTtsPlaybackCompleted = 0,
  kTtsPlaybackFailed,
  kUserBargeIn,
};

enum class ReplyGuard {
  kAlways = 0,
  kKeepSessionOpen,
  kNotKeepSessionOpen,
  kBargeInEnabled,
};

enum class ReplyAction {
  kNoop = 0,
  kRearmCommandWait,
  kEndSessionToIdle,
  kStartAsrByBargeIn,
};

struct ReplyDecision {
  ReplyState next_state = ReplyState::kResultSpeaking;
  ReplyAction action = ReplyAction::kNoop;
};

ReplyDecision StepReply(ReplyState current,
                        ReplyEvent event,
                        bool keep_session_open,
                        bool barge_in_enabled);

struct LocalEventBus {
  std::deque<WakeEvent> wake_events;
  std::deque<AsrEvent> asr_events;
  std::deque<ReplyEvent> reply_events;
};

}  // namespace mos::vis::subsm


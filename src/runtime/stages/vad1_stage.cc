#include "mos/vis/runtime/stages/vad1_stage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "Vad1Stage";

float Clamp01(float v) { return std::max(0.0F, std::min(1.0F, v)); }

// Helper to convert Vad1MachineStage to string
const char* Vad1MachineStageName(SessionContext::Vad1MachineStage stage) {
  switch (stage) {
    case SessionContext::Vad1MachineStage::kClosed:
      return "closed";
    case SessionContext::Vad1MachineStage::kOpen:
      return "open";
    case SessionContext::Vad1MachineStage::kHangover:
      return "hangover";
  }
  return "closed";
}

}  // namespace

Vad1Stage::Vad1Stage() = default;

Vad1Stage::~Vad1Stage() = default;

Status Vad1Stage::OnAttach(SessionContext& context) {
  // Initialize audio reader
  reader_ = std::make_unique<AudioReader>(context.ring_buffer.get(), "vad1");

  // Cache configuration values for performance
  const VadConfig& vad_config = context.config.vad1;
  const AudioConfig& audio_config = context.config.audio;

  vad_window_samples_ = static_cast<std::size_t>(vad_config.window_samples);
  vad_hop_samples_ = static_cast<std::size_t>(std::max(1, vad_config.hop_samples));
  capture_chunk_samples_ = static_cast<std::size_t>(
      std::max(1, audio_config.capture_chunk_samples));

  // Compute derived parameters similar to InitializeVad1StateMachine
  const int hop_samples = std::max(1, vad_config.hop_samples);
  const int sample_rate = std::max(1, audio_config.sample_rate);
  const float frame_ms = (1000.0F * static_cast<float>(hop_samples)) /
                         static_cast<float>(sample_rate);

  // Initialize VAD1 state in context
  context.vad1_state.hangover_frames = std::max(1, static_cast<int>(
      std::ceil(static_cast<float>(vad_config.hangover_ms) / frame_ms)));
  context.vad1_state.min_open_frames = std::max(1, vad_config.open_frames / 2);
  context.vad1_state.min_closed_frames = std::max(1, vad_config.close_frames / 3);
  context.vad1_state.hangover_reopen_frames = std::max(2, vad_config.open_frames / 2);
  context.vad1_state.instant_open_frames = 2;
  context.vad1_state.reopen_cooldown_frames = std::max(4, vad_config.close_frames / 2);

  context.vad1_state.open_threshold = Clamp01(vad_config.threshold);
  context.vad1_state.close_threshold = Clamp01(std::max(0.01F, vad_config.threshold * 0.55F));
  context.vad1_state.instant_open_threshold = Clamp01(
      std::max(0.70F, vad_config.threshold + 0.30F));

  // Reset state variables
  context.vad1_state.kws_gate_open = false;
  context.vad1_state.open_count = 0;
  context.vad1_state.close_count = 0;
  context.vad1_state.hangover_left = 0;
  context.vad1_state.stage_age_frames = 0;
  context.vad1_state.hangover_reopen_count = 0;
  context.vad1_state.instant_open_count = 0;
  context.vad1_state.reopen_cooldown_left = 0;
  context.vad1_state.prob_ema = 0.0F;

  // Pre-allocate buffer
  vad_window_buffer_.resize(vad_window_samples_, 0.0F);

  // Compute max windows per tick
  const std::size_t nominal_windows_per_tick =
      std::max<std::size_t>(1, capture_chunk_samples_ / vad_hop_samples_);
  max_vad_windows_per_tick_ = nominal_windows_per_tick * 4;

  GetLogger()->info(
      "{}: Initialized: open_frames={} close_frames={} hangover_ms={} "
      "hangover_frames={} min_open_frames={} min_closed_frames={} "
      "hangover_reopen_frames={} instant_open_frames={} reopen_cooldown_frames={} "
      "open_th={:.3f} close_th={:.3f} instant_open_th={:.3f}",
      kLogTag,
      std::max(1, vad_config.open_frames),
      std::max(1, vad_config.close_frames),
      vad_config.hangover_ms,
      context.vad1_state.hangover_frames,
      context.vad1_state.min_open_frames,
      context.vad1_state.min_closed_frames,
      context.vad1_state.hangover_reopen_frames,
      context.vad1_state.instant_open_frames,
      context.vad1_state.reopen_cooldown_frames,
      context.vad1_state.open_threshold,
      context.vad1_state.close_threshold,
      context.vad1_state.instant_open_threshold);

  return Status::Ok();
}

void Vad1Stage::OnDetach(SessionContext& context) {
  reader_.reset();
  vad_window_buffer_.clear();
}

bool Vad1Stage::CanProcess(SessionState state) const {
  // VAD1 stage is always active regardless of session state
  // (it needs to monitor for speech to open KWS gate)
  return true;
}

Status Vad1Stage::Process(SessionContext& context) {
  if (!context.config.vad1.enabled || context.vad1 == nullptr) {
    return Status::Ok();
  }

  std::size_t processed_windows = 0;
  while (processed_windows < max_vad_windows_per_tick_ &&
         reader_->Has(vad_window_buffer_.size()) &&
         reader_->ReadWindow(vad_window_buffer_.data(),
                             vad_window_buffer_.size(),
                             vad_hop_samples_)) {
    VadResult vad_result;
    Status st = context.vad1->Process(vad_window_buffer_.data(),
                                      vad_window_buffer_.size(),
                                      &vad_result);
    if (!st.ok()) {
      GetLogger()->error("{}: VAD-1 process failed: {}", kLogTag, st.message());
      // Continue processing other windows
    } else {
      // Update statistics in context.stats
      context.stats.vad_prob_acc += vad_result.probability;
      ++context.stats.vad_ticks;
      if (vad_result.speech) {
        ++context.stats.raw_speech_count;
      }

      // Update state machine
      bool gate_open = UpdateStateMachine(context, vad_result.probability);
      if (gate_open) {
        ++context.stats.vad_gate_open_count;
      }

      // Log debug info periodically
      if ((context.stats.vad_ticks % 50U) == 0U) {
        GetLogger()->debug("{}: prob={:.4f} speech={} gate={}",
                           kLogTag,
                           vad_result.probability,
                           vad_result.speech ? 1 : 0,
                           gate_open ? "open" : "closed");
      }
    }
    ++processed_windows;
  }

  return Status::Ok();
}

bool Vad1Stage::UpdateStateMachine(SessionContext& context, float vad_probability) {
  auto& state = context.vad1_state;

  ++state.stage_age_frames;
  if (state.reopen_cooldown_left > 0) {
    --state.reopen_cooldown_left;
  }

  const float prob = Clamp01(vad_probability);
  state.prob_ema = (state.ema_alpha * prob) + ((1.0F - state.ema_alpha) * state.prob_ema);

  const bool instant_open = prob >= state.instant_open_threshold;
  const bool above_open = state.prob_ema >= state.open_threshold;
  const bool below_close = state.prob_ema < state.close_threshold;

  const SessionContext::Vad1MachineStage prev_stage = state.stage;
  switch (state.stage) {
    case SessionContext::Vad1MachineStage::kClosed: {
      if (instant_open) {
        ++state.instant_open_count;
      } else {
        state.instant_open_count = 0;
      }
      if (instant_open || above_open) {
        ++state.open_count;
      } else {
        state.open_count = 0;
      }
      const bool can_open_now =
          state.stage_age_frames >= state.min_closed_frames && state.reopen_cooldown_left <= 0;
      if (can_open_now &&
          (state.instant_open_count >= state.instant_open_frames ||
           state.open_count >= std::max(1, context.config.vad1.open_frames))) {
        state.stage = SessionContext::Vad1MachineStage::kOpen;
        state.stage_age_frames = 0;
        state.open_count = 0;
        state.close_count = 0;
        state.hangover_left = 0;
        state.hangover_reopen_count = 0;
        state.instant_open_count = 0;
      }
      break;
    }
    case SessionContext::Vad1MachineStage::kOpen: {
      if (below_close) {
        ++state.close_count;
      } else {
        state.close_count = 0;
      }
      const bool can_leave_open = state.stage_age_frames >= state.min_open_frames;
      if (can_leave_open && state.close_count >= std::max(1, context.config.vad1.close_frames)) {
        state.stage = SessionContext::Vad1MachineStage::kHangover;
        state.stage_age_frames = 0;
        state.hangover_left = state.hangover_frames;
        state.close_count = 0;
        state.hangover_reopen_count = 0;
        state.instant_open_count = 0;
      }
      break;
    }
    case SessionContext::Vad1MachineStage::kHangover: {
      if (above_open) {
        ++state.hangover_reopen_count;
      } else {
        state.hangover_reopen_count = 0;
      }

      if ((instant_open && state.hangover_reopen_count >= 1) ||
          (state.hangover_reopen_count >= state.hangover_reopen_frames)) {
        state.stage = SessionContext::Vad1MachineStage::kOpen;
        state.stage_age_frames = 0;
        state.hangover_left = 0;
        state.open_count = 0;
        state.hangover_reopen_count = 0;
      } else {
        --state.hangover_left;
        if (state.hangover_left <= 0) {
          state.stage = SessionContext::Vad1MachineStage::kClosed;
          state.stage_age_frames = 0;
          state.hangover_left = 0;
          state.open_count = 0;
          state.hangover_reopen_count = 0;
          state.instant_open_count = 0;
          state.reopen_cooldown_left = state.reopen_cooldown_frames;
        }
      }
      break;
    }
  }

  state.kws_gate_open = (state.stage != SessionContext::Vad1MachineStage::kClosed);
  if (state.stage != prev_stage) {
    GetLogger()->debug("{}: gate transition: {} -> {} | prob={:.4f} ema={:.4f} gate={}",
                       kLogTag,
                       Vad1MachineStageName(prev_stage),
                       Vad1MachineStageName(state.stage),
                       prob,
                       state.prob_ema,
                       state.kws_gate_open ? "open" : "closed");
  }
  return state.kws_gate_open;
}

}  // namespace mos::vis

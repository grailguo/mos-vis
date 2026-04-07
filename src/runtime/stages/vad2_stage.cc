#include "mos/vis/runtime/stages/vad2_stage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "Vad2Stage";

float Clamp01(float v) { return std::max(0.0F, std::min(1.0F, v)); }

// Helper to convert Vad2MachineStage to string
const char* Vad2MachineStageName(Vad2MachineStage stage) {
  switch (stage) {
    case Vad2MachineStage::kSilence:
      return "silence";
    case Vad2MachineStage::kSpeech:
      return "speech";
  }
  return "silence";
}

}  // namespace

Vad2Stage::Vad2Stage() = default;

Vad2Stage::~Vad2Stage() = default;

Status Vad2Stage::OnAttach(SessionContext& context) {
  // Initialize audio reader
  reader_ = std::make_unique<AudioReader>(context.ring_buffer.get(), "vad2");

  // Cache configuration values for performance
  const VadConfig& vad_config = context.config.vad2;
  const AudioConfig& audio_config = context.config.audio;

  vad_window_samples_ = static_cast<std::size_t>(vad_config.window_samples);
  vad_hop_samples_ = static_cast<std::size_t>(std::max(1, vad_config.hop_samples));
  capture_chunk_samples_ = static_cast<std::size_t>(
      std::max(1, audio_config.capture_chunk_samples));

  // Compute derived parameters similar to InitializeVad2StateMachine
  const int hop_samples = std::max(1, vad_config.hop_samples);
  const int sample_rate = std::max(1, audio_config.sample_rate);
  const float frame_ms = (1000.0F * static_cast<float>(hop_samples)) /
                         static_cast<float>(sample_rate);

  // Initialize VAD2 state in context
  context.vad2_state.hangover_frames = std::max(1, static_cast<int>(
      std::ceil(static_cast<float>(vad_config.hangover_ms) / frame_ms)));
  context.vad2_state.start_threshold = Clamp01(vad_config.threshold);
  context.vad2_state.end_threshold = Clamp01(std::max(0.01F, vad_config.threshold * 0.5F));

  // Reset state variables
  context.vad2_state.stage = Vad2MachineStage::kSilence;
  context.vad2_state.start_count = 0;
  context.vad2_state.end_count = 0;
  context.vad2_state.hangover_left = 0;
  context.vad2_state.prob_ema = 0.0F;

  // Pre-allocate buffer
  vad_window_buffer_.resize(vad_window_samples_, 0.0F);

  // Compute max windows per tick
  const std::size_t nominal_windows_per_tick =
      std::max<std::size_t>(1, capture_chunk_samples_ / vad_hop_samples_);
  max_vad_windows_per_tick_ = nominal_windows_per_tick * 4;

  GetLogger()->info(
      "{}: Initialized: hangover_ms={} hangover_frames={} start_th={:.3f} end_th={:.3f}",
      kLogTag, vad_config.hangover_ms, context.vad2_state.hangover_frames,
      context.vad2_state.start_threshold, context.vad2_state.end_threshold);

  return Status::Ok();
}

void Vad2Stage::OnDetach(SessionContext& context) {
  reader_.reset();
  vad_window_buffer_.clear();
}

bool Vad2Stage::CanProcess(SessionState state) const {
  // VAD2 stage is always active; it monitors for speech boundaries regardless of session state.
  return true;
}

Status Vad2Stage::Process(SessionContext& context) {
  if (!context.config.vad2.enabled || context.vad2 == nullptr) {
    return Status::Ok();
  }

  std::size_t processed_windows = 0;
  while (processed_windows < max_vad_windows_per_tick_ &&
         reader_->Has(vad_window_buffer_.size()) &&
         reader_->ReadWindow(vad_window_buffer_.data(),
                             vad_window_buffer_.size(),
                             vad_hop_samples_)) {
    VadResult vad_result;
    Status st = context.vad2->Process(vad_window_buffer_.data(),
                                      vad_window_buffer_.size(),
                                      &vad_result);
    if (!st.ok()) {
      GetLogger()->error("{}: VAD-2 process failed: {}", kLogTag, st.message());
      break;
    }
    (void)UpdateStateMachine(context, vad_result.probability);
    ++processed_windows;
  }

  return Status::Ok();
}

bool Vad2Stage::UpdateStateMachine(SessionContext& context, float vad_probability) {
  auto& state = context.vad2_state;
  const float prob = Clamp01(vad_probability);
  state.prob_ema = (state.ema_alpha * prob) + ((1.0F - state.ema_alpha) * state.prob_ema);
  const Vad2MachineStage prev_stage = state.stage;

  if (state.stage == Vad2MachineStage::kSilence) {
    if (state.prob_ema >= state.start_threshold) {
      ++state.start_count;
    } else {
      state.start_count = 0;
    }
    if (state.start_count >= std::max(1, context.config.vad2.start_frames)) {
      state.stage = Vad2MachineStage::kSpeech;
      state.start_count = 0;
      state.end_count = 0;
      state.hangover_left = 0;
    }
  } else {
    if (state.prob_ema < state.end_threshold) {
      ++state.end_count;
    } else {
      state.end_count = 0;
      state.hangover_left = 0;
    }
    if (state.end_count >= std::max(1, context.config.vad2.end_frames)) {
      if (state.hangover_left <= 0) {
        state.hangover_left = state.hangover_frames;
      } else {
        --state.hangover_left;
      }
      if (state.hangover_left <= 0) {
        state.stage = Vad2MachineStage::kSilence;
        state.end_count = 0;
      }
    }
  }

  if (prev_stage != state.stage) {
    if (state.stage == Vad2MachineStage::kSpeech) {
      OnSpeechStart(context);
    } else {
      OnSpeechEnd(context);
    }
  }
  return state.stage == Vad2MachineStage::kSpeech;
}

void Vad2Stage::OnSpeechStart(SessionContext& context) {
  // Only trigger speech start if we are in a state that expects speech
  if (context.state == SessionState::kPreListening ||
      context.state == SessionState::kWakeCandidate) {
    context.state = SessionState::kListening;
    context.asr_state.processed_samples = 0;

    // Compute ASR start position with preroll
    const std::uint64_t write_pos = context.ring_buffer->write_pos();
    const std::uint64_t oldest = context.ring_buffer->OldestPos();
    const std::size_t preroll_samples = static_cast<std::size_t>(
        std::max(0, context.config.asr.preroll_ms) *
        std::max(1, context.config.audio.sample_rate) / 1000);
    const std::uint64_t start_pos =
        (write_pos > preroll_samples) ? (write_pos - preroll_samples) : 0;
    const std::uint64_t clamped_start_pos = std::max(oldest, start_pos);

    // ASR reader position is managed by AsrStage, but we need to store the wake position
    context.nlu_control_state.wake_pos_samples = clamped_start_pos;

    context.asr_state.last_partial_text.clear();
    context.asr_state.last_text_update_time = std::chrono::steady_clock::now();

    // Reset ASR engine if available
    if (context.asr != nullptr && context.config.asr.enabled) {
      Status st = context.asr->Reset();
      if (!st.ok()) {
        GetLogger()->warn("{}: ASR reset failed: {}", kLogTag, st.message());
      }
    }

    GetLogger()->debug("{}: speech start, ASR preroll={} samples", kLogTag, preroll_samples);
  }
}

void Vad2Stage::OnSpeechEnd(SessionContext& context) {
  if (context.state == SessionState::kListening) {
    context.state = SessionState::kFinalizing;
    GetLogger()->debug("{}: speech end, finalizing ASR", kLogTag);
  }
}

}  // namespace mos::vis
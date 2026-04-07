
#include "mos/vis/runtime/session_controller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mos/vis/common/logging.h"

namespace mos::vis {
namespace {

struct ChunkStats {
  float rms = 0.0F;
  float peak = 0.0F;
};

ChunkStats ComputeChunkStats(const std::vector<float>& samples) {
  ChunkStats stats;
  if (samples.empty()) {
    return stats;
  }

  double sum = 0.0;
  float peak = 0.0F;
  for (float x : samples) {
    sum += static_cast<double>(x) * static_cast<double>(x);
    peak = std::max(peak, std::abs(x));
  }

  stats.rms = static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
  stats.peak = peak;
  return stats;
}

const char* Vad1StageName(SessionController::Vad1Stage stage) {
  switch (stage) {
    case SessionController::Vad1Stage::kClosed:
      return "closed";
    case SessionController::Vad1Stage::kOpen:
      return "open";
    case SessionController::Vad1Stage::kHangover:
      return "hangover";
  }
  return "closed";
}

const char* Vad2StageName(SessionController::Vad2Stage stage) {
  switch (stage) {
    case SessionController::Vad2Stage::kSilence:
      return "silence";
    case SessionController::Vad2Stage::kSpeech:
      return "speech";
  }
  return "silence";
}

float Clamp01(float v) { return std::max(0.0F, std::min(1.0F, v)); }

std::string TrimAsciiWhitespace(const std::string& s) {
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

}  // namespace

SessionController::SessionController(const AppConfig& config,
                                     std::shared_ptr<AudioRingBuffer> ring)
    : config_(config),
      ring_(std::move(ring)),
      vad1_reader_(ring_.get(), "vad1"),
      kws_reader_(ring_.get(), "kws"),
      vad2_reader_(ring_.get(), "vad2"),
      asr_reader_(ring_.get(), "asr") {
  InitializeVad1StateMachine();
  InitializeVad2StateMachine();
  kws_preroll_capacity_samples_ = static_cast<std::size_t>(
      std::max(0, config_.kws.preroll_ms) * std::max(1, config_.audio.sample_rate) / 1000);
  kws_pending_max_age_chunks_ =
      std::max(1, (std::max(200, config_.kws.preroll_ms) / 20));  // Tick is 20ms.
}

Status SessionController::Initialize() {
  vad1_ = CreateVadEngine();
  if (config_.vad1.enabled) {
    Status st = vad1_->Initialize(config_.vad1);
    if (!st.ok()) {
      return st;
    }
  }

  vad2_ = CreateVadEngine();
  if (config_.vad2.enabled) {
    Status st = vad2_->Initialize(config_.vad2);
    if (!st.ok()) {
      return st;
    }
  }

  kws_ = CreateKwsEngine();
  if (config_.kws.enabled) {
    Status st = kws_->Initialize(config_.kws);
    if (!st.ok()) {
      return st;
    }
  }

  asr_ = CreateAsrEngine();
  if (config_.asr.enabled) {
    Status st = asr_->Initialize(config_.asr);
    if (!st.ok()) {
      return st;
    }
  }

  nlu_ = CreateNluEngine();
  {
    NluConfig nlu_config;
    Status st = nlu_->Initialize(nlu_config);
    if (!st.ok()) {
      return st;
    }
  }

  control_ = CreateControlEngine(config_.control);
  {
    Status st = control_->Initialize();
    if (!st.ok()) {
      return st;
    }
  }

  tts_ = CreateTtsEngine();
  if (config_.tts.enabled) {
    Status st = tts_->Initialize(config_.tts, config_.audio);
    if (!st.ok()) {
      return st;
    }
  }
  GetLogger()->info("wait for awake");
  return Status::Ok();
}

std::string SessionController::ResolveWakeAckText(const std::string& keyword) const {
  for (const auto& rule : config_.wake_ack_text) {
    for (const auto& kw : rule.keywords) {
      if (kw == keyword) {
        return rule.reply_text;
      }
    }
  }
  return "";
}

std::string SessionController::ResolveWakeAckPresetFile(const std::string& keyword) const {
  for (const auto& rule : config_.wake_ack_text) {
    for (const auto& kw : rule.keywords) {
      if (kw == keyword) {
        return rule.preset_file;
      }
    }
  }
  return "";
}

void SessionController::HandleKwsHit(const std::string& keyword, const std::string& detail_json) {
  state_ = SessionState::kWakeCandidate;
  last_wake_keyword_ = keyword;
  last_wake_ack_text_ = ResolveWakeAckText(keyword);
  last_wake_ack_preset_file_ = ResolveWakeAckPresetFile(keyword);

  (void)keyword;
  (void)detail_json;
  tts_tasks_.push_back(TtsTask{last_wake_ack_preset_file_, last_wake_ack_text_});
  wake_ack_pending_ = true;
  GetLogger()->info("wakeup hit: {}", keyword);
  if (!detail_json.empty()) {
    GetLogger()->debug("kws detail: {}", detail_json);
  }

  state_ = SessionState::kPreListening;
  GetLogger()->info("wait for instruction");
}

void SessionController::InitializeVad1StateMachine() {
  vad1_stage_ = Vad1Stage::kClosed;
  vad1_kws_gate_open_ = false;
  vad1_open_count_ = 0;
  vad1_close_count_ = 0;
  vad1_hangover_left_ = 0;
  vad1_stage_age_frames_ = 0;
  vad1_hangover_reopen_count_ = 0;
  vad1_instant_open_count_ = 0;
  vad1_reopen_cooldown_left_ = 0;
  vad1_prob_ema_ = 0.0F;

  const int hop_samples = std::max(1, config_.vad1.hop_samples);
  const int sample_rate = std::max(1, config_.audio.sample_rate);
  const float frame_ms = (1000.0F * static_cast<float>(hop_samples)) /
                         static_cast<float>(sample_rate);
  vad1_hangover_frames_ =
      std::max(1, static_cast<int>(std::ceil(static_cast<float>(config_.vad1.hangover_ms) / frame_ms)));
  vad1_min_open_frames_ = std::max(1, config_.vad1.open_frames / 2);
  vad1_min_closed_frames_ = std::max(1, config_.vad1.close_frames / 3);
  vad1_hangover_reopen_frames_ = std::max(2, config_.vad1.open_frames / 2);
  vad1_instant_open_frames_ = 2;
  vad1_reopen_cooldown_frames_ = std::max(4, config_.vad1.close_frames / 2);

  vad1_open_threshold_ = Clamp01(config_.vad1.threshold);
  vad1_close_threshold_ = Clamp01(std::max(0.01F, config_.vad1.threshold * 0.55F));
  vad1_instant_open_threshold_ = Clamp01(std::max(0.70F, config_.vad1.threshold + 0.30F));

  GetLogger()->info(
      "VAD-1 state machine initialized: open_frames={} close_frames={} hangover_ms={} "
      "hangover_frames={} min_open_frames={} min_closed_frames={} "
      "hangover_reopen_frames={} instant_open_frames={} reopen_cooldown_frames={} "
      "ema_alpha={:.2f} open_th={:.3f} close_th={:.3f} "
      "instant_open_th={:.3f}",
      std::max(1, config_.vad1.open_frames),
      std::max(1, config_.vad1.close_frames),
      config_.vad1.hangover_ms,
      vad1_hangover_frames_,
      vad1_min_open_frames_,
      vad1_min_closed_frames_,
      vad1_hangover_reopen_frames_,
      vad1_instant_open_frames_,
      vad1_reopen_cooldown_frames_,
      vad1_ema_alpha_,
      vad1_open_threshold_,
      vad1_close_threshold_,
      vad1_instant_open_threshold_);
}

void SessionController::InitializeVad2StateMachine() {
  vad2_stage_ = Vad2Stage::kSilence;
  vad2_start_count_ = 0;
  vad2_end_count_ = 0;
  vad2_hangover_left_ = 0;
  vad2_prob_ema_ = 0.0F;

  const int hop_samples = std::max(1, config_.vad2.hop_samples);
  const int sample_rate = std::max(1, config_.audio.sample_rate);
  const float frame_ms =
      (1000.0F * static_cast<float>(hop_samples)) / static_cast<float>(sample_rate);
  vad2_hangover_frames_ =
      std::max(1, static_cast<int>(std::ceil(static_cast<float>(config_.vad2.hangover_ms) / frame_ms)));
  vad2_start_threshold_ = Clamp01(config_.vad2.threshold);
  vad2_end_threshold_ = Clamp01(std::max(0.05F, config_.vad2.threshold * 0.6F));

  GetLogger()->info(
      "VAD-2 state machine initialized: start_frames={} end_frames={} hangover_ms={} "
      "hangover_frames={} start_th={:.3f} end_th={:.3f}",
      std::max(1, config_.vad2.start_frames),
      std::max(1, config_.vad2.end_frames),
      config_.vad2.hangover_ms,
      vad2_hangover_frames_,
      vad2_start_threshold_,
      vad2_end_threshold_);
}

bool SessionController::UpdateVad1StateMachine(float vad_probability) {
  ++vad1_stage_age_frames_;
  if (vad1_reopen_cooldown_left_ > 0) {
    --vad1_reopen_cooldown_left_;
  }
  const float prob = Clamp01(vad_probability);
  vad1_prob_ema_ = (vad1_ema_alpha_ * prob) + ((1.0F - vad1_ema_alpha_) * vad1_prob_ema_);

  const bool instant_open = prob >= vad1_instant_open_threshold_;
  const bool above_open = vad1_prob_ema_ >= vad1_open_threshold_;
  const bool below_close = vad1_prob_ema_ < vad1_close_threshold_;

  const Vad1Stage prev_stage = vad1_stage_;
  switch (vad1_stage_) {
    case Vad1Stage::kClosed: {
      if (instant_open) {
        ++vad1_instant_open_count_;
      } else {
        vad1_instant_open_count_ = 0;
      }
      if (instant_open || above_open) {
        ++vad1_open_count_;
      } else {
        vad1_open_count_ = 0;
      }
      const bool can_open_now =
          vad1_stage_age_frames_ >= vad1_min_closed_frames_ && vad1_reopen_cooldown_left_ <= 0;
      if (can_open_now &&
          (vad1_instant_open_count_ >= vad1_instant_open_frames_ ||
           vad1_open_count_ >= std::max(1, config_.vad1.open_frames))) {
        vad1_stage_ = Vad1Stage::kOpen;
        vad1_stage_age_frames_ = 0;
        vad1_open_count_ = 0;
        vad1_close_count_ = 0;
        vad1_hangover_left_ = 0;
        vad1_hangover_reopen_count_ = 0;
        vad1_instant_open_count_ = 0;
      }
      break;
    }
    case Vad1Stage::kOpen: {
      if (below_close) {
        ++vad1_close_count_;
      } else {
        vad1_close_count_ = 0;
      }
      const bool can_leave_open = vad1_stage_age_frames_ >= vad1_min_open_frames_;
      if (can_leave_open && vad1_close_count_ >= std::max(1, config_.vad1.close_frames)) {
        vad1_stage_ = Vad1Stage::kHangover;
        vad1_stage_age_frames_ = 0;
        vad1_hangover_left_ = vad1_hangover_frames_;
        vad1_close_count_ = 0;
        vad1_hangover_reopen_count_ = 0;
        vad1_instant_open_count_ = 0;
      }
      break;
    }
    case Vad1Stage::kHangover: {
      if (above_open) {
        ++vad1_hangover_reopen_count_;
      } else {
        vad1_hangover_reopen_count_ = 0;
      }

      if ((instant_open && vad1_hangover_reopen_count_ >= 1) ||
          (vad1_hangover_reopen_count_ >= vad1_hangover_reopen_frames_)) {
        vad1_stage_ = Vad1Stage::kOpen;
        vad1_stage_age_frames_ = 0;
        vad1_hangover_left_ = 0;
        vad1_open_count_ = 0;
        vad1_hangover_reopen_count_ = 0;
      } else {
        --vad1_hangover_left_;
        if (vad1_hangover_left_ <= 0) {
          vad1_stage_ = Vad1Stage::kClosed;
          vad1_stage_age_frames_ = 0;
          vad1_hangover_left_ = 0;
          vad1_open_count_ = 0;
          vad1_hangover_reopen_count_ = 0;
          vad1_instant_open_count_ = 0;
          vad1_reopen_cooldown_left_ = vad1_reopen_cooldown_frames_;
        }
      }
      break;
    }
  }

  vad1_kws_gate_open_ = (vad1_stage_ != Vad1Stage::kClosed);
  if (vad1_stage_ != prev_stage) {
    GetLogger()->debug("vad1 gate transition: {} -> {} | prob={:.4f} ema={:.4f} gate={}",
                       Vad1StageName(prev_stage),
                       Vad1StageName(vad1_stage_),
                       prob,
                       vad1_prob_ema_,
                       vad1_kws_gate_open_ ? "open" : "closed");
  }
  return vad1_kws_gate_open_;
}

bool SessionController::UpdateVad2StateMachine(float vad_probability) {
  const float prob = Clamp01(vad_probability);
  vad2_prob_ema_ = (vad2_ema_alpha_ * prob) + ((1.0F - vad2_ema_alpha_) * vad2_prob_ema_);
  const Vad2Stage prev = vad2_stage_;

  if (vad2_stage_ == Vad2Stage::kSilence) {
    if (vad2_prob_ema_ >= vad2_start_threshold_) {
      ++vad2_start_count_;
    } else {
      vad2_start_count_ = 0;
    }
    if (vad2_start_count_ >= std::max(1, config_.vad2.start_frames)) {
      vad2_stage_ = Vad2Stage::kSpeech;
      vad2_start_count_ = 0;
      vad2_end_count_ = 0;
      vad2_hangover_left_ = 0;
    }
  } else {
    if (vad2_prob_ema_ < vad2_end_threshold_) {
      ++vad2_end_count_;
    } else {
      vad2_end_count_ = 0;
      vad2_hangover_left_ = 0;
    }
    if (vad2_end_count_ >= std::max(1, config_.vad2.end_frames)) {
      if (vad2_hangover_left_ <= 0) {
        vad2_hangover_left_ = vad2_hangover_frames_;
      } else {
        --vad2_hangover_left_;
      }
      if (vad2_hangover_left_ <= 0) {
        vad2_stage_ = Vad2Stage::kSilence;
        vad2_end_count_ = 0;
      }
    }
  }

  if (prev != vad2_stage_) {
    if (vad2_stage_ == Vad2Stage::kSpeech) {
      OnVad2SpeechStart();
    } else {
      OnVad2SpeechEnd();
    }
  }
  return vad2_stage_ == Vad2Stage::kSpeech;
}

void SessionController::OnVad2SpeechStart() {
  if (state_ == SessionState::kPreListening || state_ == SessionState::kWakeCandidate) {
    state_ = SessionState::kListening;
    asr_processed_samples_ = 0;
    const std::uint64_t write_pos = ring_->write_pos();
    const std::uint64_t oldest = ring_->OldestPos();
    const std::size_t preroll_samples = static_cast<std::size_t>(
        std::max(0, config_.asr.preroll_ms) * std::max(1, config_.audio.sample_rate) / 1000);
    const std::uint64_t start_pos =
        (write_pos > preroll_samples) ? (write_pos - preroll_samples) : 0;
    const std::uint64_t clamped_start_pos = std::max(oldest, start_pos);
    asr_reader_.Seek(clamped_start_pos);
    wake_pos_samples_ = clamped_start_pos;
    last_partial_asr_text_.clear();
    asr_last_text_update_time_ = std::chrono::steady_clock::now();
    if (asr_ != nullptr && config_.asr.enabled) {
      const Status st = asr_->Reset();
      if (!st.ok()) {
        GetLogger()->warn("ASR reset failed: {}", st.message());
      }
    }
  }
}

void SessionController::OnVad2SpeechEnd() {
  if (state_ == SessionState::kListening) {
    state_ = SessionState::kFinalizing;
  }
}

void SessionController::ProcessVad1Stage(TickStats* stats) {
  if (!config_.vad1.enabled || vad1_ == nullptr) {
    return;
  }
  const std::size_t vad_window_samples = static_cast<std::size_t>(config_.vad1.window_samples);
  const std::size_t vad_hop_samples = static_cast<std::size_t>(std::max(1, config_.vad1.hop_samples));
  const std::size_t capture_chunk_samples =
      static_cast<std::size_t>(std::max(1, config_.audio.capture_chunk_samples));
  const std::size_t nominal_windows_per_tick =
      std::max<std::size_t>(1, capture_chunk_samples / vad_hop_samples);
  const std::size_t max_vad_windows_per_tick = nominal_windows_per_tick * 4;

  std::vector<float> vad_window(vad_window_samples, 0.0F);
  std::size_t processed_windows = 0;
  while (processed_windows < max_vad_windows_per_tick &&
         vad1_reader_.Has(vad_window.size()) &&
         vad1_reader_.ReadWindow(vad_window.data(), vad_window_samples, vad_hop_samples)) {
    VadResult vad_result;
    Status st = vad1_->Process(vad_window.data(), vad_window.size(), &vad_result);
    if (!st.ok()) {
      GetLogger()->error("VAD-1 process failed: {}", st.message());
    } else {
      if ((stats->vad_ticks % 50U) == 0U) {
        GetLogger()->debug("vad1 prob={:.4f} speech={} gate={}",
                           vad_result.probability,
                           vad_result.speech ? 1 : 0,
                           vad1_kws_gate_open_ ? "open" : "closed");
      }
      stats->vad_prob_acc += vad_result.probability;
      ++stats->vad_ticks;
      if (vad_result.speech) {
        ++stats->raw_speech_count;
      }
      if (UpdateVad1StateMachine(vad_result.probability)) {
        ++stats->vad_gate_open_count;
      }
    }
    ++processed_windows;
  }
}

void SessionController::ProcessVad2Stage() {
  if (!config_.vad2.enabled || vad2_ == nullptr) {
    return;
  }
  const std::size_t vad_window_samples = static_cast<std::size_t>(config_.vad2.window_samples);
  const std::size_t vad_hop_samples = static_cast<std::size_t>(std::max(1, config_.vad2.hop_samples));
  const std::size_t capture_chunk_samples =
      static_cast<std::size_t>(std::max(1, config_.audio.capture_chunk_samples));
  const std::size_t nominal_windows_per_tick =
      std::max<std::size_t>(1, capture_chunk_samples / vad_hop_samples);
  const std::size_t max_vad_windows_per_tick = nominal_windows_per_tick * 4;

  std::vector<float> vad_window(vad_window_samples, 0.0F);
  std::size_t processed_windows = 0;
  while (processed_windows < max_vad_windows_per_tick &&
         vad2_reader_.Has(vad_window.size()) &&
         vad2_reader_.ReadWindow(vad_window.data(), vad_window_samples, vad_hop_samples)) {
    VadResult vad_result;
    Status st = vad2_->Process(vad_window.data(), vad_window.size(), &vad_result);
    if (!st.ok()) {
      GetLogger()->error("VAD-2 process failed: {}", st.message());
      break;
    }
    (void)UpdateVad2StateMachine(vad_result.probability);
    ++processed_windows;
  }
}

void SessionController::ProcessAsrStage() {
  if (!config_.asr.enabled || asr_ == nullptr) {
    return;
  }
  if (state_ != SessionState::kListening && state_ != SessionState::kFinalizing) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if ((now - asr_last_text_update_time_) >=
      std::chrono::seconds(std::max(1, asr_no_text_timeout_seconds_))) {
    GetLogger()->warn("ASR no new text for {}s, force session -> kIdle",
                      asr_no_text_timeout_seconds_);
    if (asr_ != nullptr && config_.asr.enabled) {
      const Status st = asr_->Reset();
      if (!st.ok()) {
        GetLogger()->warn("ASR reset failed on timeout: {}", st.message());
      }
    }
    state_ = SessionState::kIdle;
    wake_pos_samples_.reset();
    asr_processed_samples_ = 0;
    last_partial_asr_text_.clear();
    has_pending_asr_final_result_ = false;
    pending_asr_final_result_ = AsrResult{};
    has_pending_nlu_result_ = false;
    pending_nlu_result_ = NluResult{};
    pending_control_text_.clear();
    GetLogger()->info("wait for awake");
    return;
  }

  const std::size_t asr_chunk = static_cast<std::size_t>(std::max(1, config_.asr.chunk_samples));
  const std::size_t max_chunks_per_tick = 8;
  std::vector<float> chunk(asr_chunk, 0.0F);
  std::size_t consumed_chunks = 0;
  while (consumed_chunks < max_chunks_per_tick &&
         asr_reader_.Has(chunk.size()) &&
         asr_reader_.ReadAndAdvance(chunk.data(), chunk.size())) {
    asr_processed_samples_ += chunk.size();
    Status st = asr_->AcceptAudio(chunk.data(), chunk.size());
    if (!st.ok()) {
      GetLogger()->error("ASR accept failed: {}", st.message());
      break;
    }
    st = asr_->DecodeAvailable();
    if (!st.ok()) {
      GetLogger()->error("ASR decode failed: {}", st.message());
      break;
    }

    AsrResult partial;
    st = asr_->GetResult(&partial);
    if (st.ok() && !partial.text.empty() && partial.text != last_partial_asr_text_) {
      last_partial_asr_text_ = partial.text;
      asr_last_text_update_time_ = std::chrono::steady_clock::now();
    }
    ++consumed_chunks;
  }

  if (state_ == SessionState::kFinalizing) {
    const std::size_t tail_samples = static_cast<std::size_t>(
        std::max(0, config_.asr.tail_ms) * std::max(1, config_.audio.sample_rate) / 1000);
    std::size_t extra_samples = 0;
    while (extra_samples < tail_samples &&
           asr_reader_.Has(chunk.size()) &&
           asr_reader_.ReadAndAdvance(chunk.data(), chunk.size())) {
      asr_processed_samples_ += chunk.size();
      extra_samples += chunk.size();
      Status st = asr_->AcceptAudio(chunk.data(), chunk.size());
      if (!st.ok()) {
        GetLogger()->error("ASR accept failed while tailing: {}", st.message());
        break;
      }
      st = asr_->DecodeAvailable();
      if (!st.ok()) {
        GetLogger()->error("ASR decode failed while tailing: {}", st.message());
        break;
      }
    }

    AsrResult final_result;
    Status st = asr_->FinalizeAndFlush(&final_result);
    if (!st.ok()) {
      GetLogger()->error("ASR finalize failed: {}", st.message());
    }

    std::string final_text = TrimAsciiWhitespace(final_result.text);
    if (final_text.empty()) {
      const std::string partial_text = TrimAsciiWhitespace(last_partial_asr_text_);
      if (!partial_text.empty()) {
        final_result.text = partial_text;
        final_text = partial_text;
      }
    }

    if (final_text.empty()) {
      GetLogger()->warn("ASR final text is empty, skip recognizing and return to pre-listening");
      state_ = SessionState::kPreListening;
      GetLogger()->info("wait for instruction");
      wake_pos_samples_.reset();
      asr_processed_samples_ = 0;
      last_partial_asr_text_.clear();
      return;
    }

    pending_asr_final_result_ = std::move(final_result);
    has_pending_asr_final_result_ = true;
    state_ = SessionState::kRecognizing;
    wake_pos_samples_.reset();
    asr_processed_samples_ = 0;
    last_partial_asr_text_.clear();
  }
}

void SessionController::ProcessRecognizingStage() {
  if (state_ != SessionState::kRecognizing) {
    return;
  }

  if (!has_pending_asr_final_result_) {
    GetLogger()->warn("Recognizing stage entered without pending ASR result");
    state_ = SessionState::kIdle;
    GetLogger()->info("wait for awake");
    return;
  }

  const AsrResult& final_result = pending_asr_final_result_;
  const std::string final_text = TrimAsciiWhitespace(final_result.text);
  if (final_text.empty()) {
    GetLogger()->warn("Recognizing skipped: final ASR text is empty");
    has_pending_asr_final_result_ = false;
    pending_asr_final_result_ = AsrResult{};
    state_ = SessionState::kPreListening;
    GetLogger()->info("wait for instruction");
    return;
  }
  GetLogger()->info("ASR final text: {}", final_text);

  pending_nlu_result_ = NluResult{};
  has_pending_nlu_result_ = false;
  pending_control_text_ = final_text;
  if (nlu_ != nullptr) {
    Status st = nlu_->Reset();
    if (!st.ok()) {
      GetLogger()->warn("NLU reset failed: {}", st.message());
    }
    NluResult nlu_result;
    st = nlu_->Infer(final_result.text, &nlu_result);
    if (!st.ok()) {
      GetLogger()->warn("NLU infer failed: {}", st.message());
    } else {
      GetLogger()->info("NLU result: intent={} confidence={:.3f}",
                        nlu_result.intent,
                        nlu_result.confidence);
      if (nlu_result.intent.rfind("device.control.", 0) == 0) {
        GetLogger()->info("NLU matched control command: {}", nlu_result.intent);
      }
      pending_nlu_result_ = std::move(nlu_result);
      has_pending_nlu_result_ = true;
    }
  }

  state_ = SessionState::kExecuting;

  has_pending_asr_final_result_ = false;
  pending_asr_final_result_ = AsrResult{};
}

void SessionController::ProcessExecutingStage() {
  if (state_ != SessionState::kExecuting) {
    return;
  }

  std::string reply_text;
  bool is_unknown_intent = true;
  bool is_control_intent = false;
  ControlRequest control_request;
  if (has_pending_nlu_result_) {
    control_request.intent = pending_nlu_result_.intent;
    control_request.confidence = pending_nlu_result_.confidence;
    control_request.text = pending_control_text_;
    control_request.nlu_json = pending_nlu_result_.json;
    is_control_intent = control_request.intent.rfind("device.control.", 0) == 0;
    if (!is_control_intent) {
      reply_text = pending_nlu_result_.reply_text;
    }
    is_unknown_intent = pending_nlu_result_.intent == "unknown";
  }

  ControlResult control_result;
  const bool should_execute_control =
      has_pending_nlu_result_ &&
      !control_request.intent.empty() &&
      control_request.intent != "unknown" &&
      control_request.intent.rfind("device.control.", 0) == 0;
  if (should_execute_control && control_ != nullptr) {
    GetLogger()->info("Control execute begin: intent={} text={}",
                      control_request.intent,
                      control_request.text);
    Status st = control_->Reset();
    if (!st.ok()) {
      GetLogger()->warn("Control reset failed: {}", st.message());
    }
    st = control_->Execute(control_request, &control_result);
    if (!st.ok()) {
      GetLogger()->warn("Control execute failed: {}", st.message());
      reply_text = "控制请求失败：" + st.message();
    } else {
      GetLogger()->info("Control execute done: handled={} action={} reply={}",
                        control_result.handled ? 1 : 0,
                        control_result.action,
                        control_result.reply_text);
      reply_text = control_result.reply_text;
      if (!control_result.handled && is_unknown_intent == false) {
        // Known intent from NLU but no control action implemented yet.
        is_unknown_intent = true;
      }
    }
  } else if (has_pending_nlu_result_) {
    GetLogger()->info("Control execute skipped: intent={}", control_request.intent);
  }

  if (!reply_text.empty()) {
    tts_tasks_.push_back(TtsTask{"", reply_text});
    has_pending_nlu_result_ = false;
    pending_nlu_result_ = NluResult{};
    pending_control_text_.clear();
    return;
  }

  if (is_unknown_intent) {
    has_pending_nlu_result_ = false;
    pending_nlu_result_ = NluResult{};
    pending_control_text_.clear();
    state_ = SessionState::kPreListening;
    GetLogger()->info("wait for instruction");
    return;
  }

  state_ = SessionState::kThinking;
  has_pending_nlu_result_ = false;
  pending_nlu_result_ = NluResult{};
  pending_control_text_.clear();
  state_ = SessionState::kIdle;
  GetLogger()->info("wait for awake");
}

void SessionController::ProcessControlNotificationStage() {
  if (control_ == nullptr) {
    return;
  }
  ControlResult notify_result;
  Status st = control_->PollNotification(&notify_result);
  if (!st.ok()) {
    GetLogger()->warn("Control notification poll failed: {}", st.message());
    return;
  }
  if (!notify_result.reply_text.empty()) {
    GetLogger()->info("Control notify: action={} reply={}",
                      notify_result.action,
                      notify_result.reply_text);
    tts_tasks_.push_back(TtsTask{"", notify_result.reply_text});
  }
}

void SessionController::ProcessTtsStage() {
  if (!config_.tts.enabled || tts_ == nullptr) {
    return;
  }
  if (tts_task_running_ || tts_tasks_.empty()) {
    return;
  }
  const TtsTask task = tts_tasks_.front();
  tts_tasks_.pop_front();
  tts_task_running_ = true;
  state_ = SessionState::kSpeaking;
  Status st = Status::Ok();
  if (!task.preset_file.empty()) {
    st = tts_->PlayFile(task.preset_file);
  } else if (!task.reply_text.empty()) {
    st = tts_->Speak(task.reply_text);
  }
  if (!st.ok()) {
    GetLogger()->warn("TTS task failed: {}", st.message());
  }
  wake_ack_pending_ = false;
  if (has_pending_nlu_result_) {
    state_ = SessionState::kThinking;
    has_pending_nlu_result_ = false;
    pending_nlu_result_ = NluResult{};
    pending_control_text_.clear();
    state_ = SessionState::kIdle;
    GetLogger()->info("wait for awake");
  } else {
    state_ = SessionState::kPreListening;
    GetLogger()->info("wait for instruction");
  }
  tts_task_running_ = false;
}

void SessionController::ProcessKwsStage(TickStats* stats) {
  if (!config_.kws.enabled || kws_ == nullptr) {
    return;
  }

  const bool gate_just_opened = vad1_kws_gate_open_ && !prev_vad1_kws_gate_open_;
  const bool gate_just_closed = !vad1_kws_gate_open_ && prev_vad1_kws_gate_open_;
  if (gate_just_opened) {
    {
      const Status reset_st = kws_->Reset();
      if (!reset_st.ok()) {
        GetLogger()->warn("KWS reset failed on gate open: {}", reset_st.message());
      } else {
        GetLogger()->debug("kws stream reset on gate open");
      }
    }
    kws_fired_in_current_gate_ = false;

    if (kws_pending_hit_ && !kws_fired_in_current_gate_) {
      ++stats->kws_hit_count;
      kws_fired_in_current_gate_ = true;
      HandleKwsHit(kws_pending_keyword_, kws_pending_json_);
      kws_pending_hit_ = false;
      kws_pending_keyword_.clear();
      kws_pending_json_.clear();
      kws_pending_age_chunks_ = 0;
    }
  }
  if (gate_just_closed) {
    const Status reset_st = kws_->Reset();
    if (!reset_st.ok()) {
      GetLogger()->warn("KWS reset failed on gate close: {}", reset_st.message());
    } else {
      GetLogger()->debug("kws stream reset on gate close");
    }
  }

  const std::size_t kws_chunk_samples = static_cast<std::size_t>(std::max(1, config_.kws.chunk_samples));
  const std::size_t capture_chunk_samples =
      static_cast<std::size_t>(std::max(1, config_.audio.capture_chunk_samples));
  const std::size_t nominal_chunks_per_tick =
      std::max<std::size_t>(1, capture_chunk_samples / kws_chunk_samples);
  const std::size_t max_kws_chunks_per_tick = nominal_chunks_per_tick * 8;

  std::vector<float> chunk(kws_chunk_samples, 0.0F);
  std::size_t processed_chunks = 0;
  while (processed_chunks < max_kws_chunks_per_tick &&
         kws_reader_.Has(chunk.size()) &&
         kws_reader_.ReadAndAdvance(chunk.data(), chunk.size())) {
    const ChunkStats chunk_stats = ComputeChunkStats(chunk);
    stats->rms_acc += chunk_stats.rms;
    stats->peak_acc += chunk_stats.peak;
    ++stats->chunk_ticks;
    if ((stats->chunk_ticks % 50U) == 0U) {
      GetLogger()->debug("kws input: rms={:.4f} peak={:.4f} gate={} state={}",
                         chunk_stats.rms,
                         chunk_stats.peak,
                         vad1_kws_gate_open_ ? "open" : "closed",
                         static_cast<int>(state_));
    }

    if (kws_preroll_capacity_samples_ > 0U) {
      for (float s : chunk) {
        kws_preroll_samples_.push_back(s);
        if (kws_preroll_samples_.size() > kws_preroll_capacity_samples_) {
          kws_preroll_samples_.pop_front();
        }
      }
    }

    if (kws_pending_hit_) {
      ++kws_pending_age_chunks_;
      if (kws_pending_age_chunks_ > kws_pending_max_age_chunks_) {
        kws_pending_hit_ = false;
        kws_pending_keyword_.clear();
        kws_pending_json_.clear();
        kws_pending_age_chunks_ = 0;
      }
    }

    KwsResult kws_result;
    Status st = kws_->Process(chunk.data(), chunk.size(), &kws_result);
    if (!st.ok()) {
      GetLogger()->error("KWS process failed: {}", st.message());
    } else if (kws_result.detected) {
      GetLogger()->debug("kws detected: keyword='{}' gate={} state={}",
                         kws_result.keyword,
                         vad1_kws_gate_open_ ? "open" : "closed",
                         static_cast<int>(state_));

      // Bypass VAD-1 gate: wake immediately on KWS hit while waiting for wakeup.
      if (state_ == SessionState::kIdle) {
        ++stats->kws_hit_count;
        kws_fired_in_current_gate_ = true;
        kws_pending_hit_ = false;
        kws_pending_keyword_.clear();
        kws_pending_json_.clear();
        kws_pending_age_chunks_ = 0;
        HandleKwsHit(kws_result.keyword, kws_result.json);
      } else {
        GetLogger()->debug("kws hit ignored: state={} (not idle)", static_cast<int>(state_));
      }
    }
    ++processed_chunks;
  }

  if (processed_chunks == 0U) {
    ++kws_empty_read_ticks_;
    if ((kws_empty_read_ticks_ % 50U) == 0U) {
      GetLogger()->debug("kws idle: no chunks (kws_pos={} write_pos={} oldest_pos={})",
                         kws_reader_.pos(),
                         ring_->write_pos(),
                         ring_->OldestPos());
    }
  } else {
    kws_empty_read_ticks_ = 0;
  }

  prev_vad1_kws_gate_open_ = vad1_kws_gate_open_;
}

void SessionController::EmitFrontendStats(TickStats* stats) {
  (void)stats;
}

void SessionController::Tick() {
  ProcessControlNotificationStage();
  ProcessVad1Stage(&stats_);
  ProcessKwsStage(&stats_);
  ProcessVad2Stage();
  ProcessRecognizingStage();
  ProcessExecutingStage();
  ProcessAsrStage();
  ProcessTtsStage();
  EmitFrontendStats(&stats_);
}

SessionState SessionController::state() const { return state_; }

}  // namespace mos::vis

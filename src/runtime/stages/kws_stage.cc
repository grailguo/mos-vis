#include "mos/vis/runtime/stages/kws_stage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "KwsStage";

// Helper to compute chunk statistics (RMS and peak)
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

// Helper to resolve wake acknowledgment text from configuration
std::string ResolveWakeAckText(const AppConfig& config, const std::string& keyword) {
  for (const auto& rule : config.wake_ack_text) {
    for (const auto& kw : rule.keywords) {
      if (kw == keyword) {
        return rule.reply_text;
      }
    }
  }
  return "";
}

// Helper to resolve wake acknowledgment preset file from configuration
std::string ResolveWakeAckPresetFile(const AppConfig& config, const std::string& keyword) {
  for (const auto& rule : config.wake_ack_text) {
    for (const auto& kw : rule.keywords) {
      if (kw == keyword) {
        return rule.preset_file;
      }
    }
  }
  return "";
}

}  // namespace

KwsStage::KwsStage() = default;

KwsStage::~KwsStage() = default;

Status KwsStage::OnAttach(SessionContext& context) {
  // Initialize audio reader
  reader_ = std::make_unique<AudioReader>(context.ring_buffer.get(), "kws");

  // Cache configuration values for performance
  const KwsConfig& kws_config = context.config.kws;
  const AudioConfig& audio_config = context.config.audio;

  kws_chunk_samples_ = static_cast<std::size_t>(std::max(1, kws_config.chunk_samples));
  capture_chunk_samples_ = static_cast<std::size_t>(
      std::max(1, audio_config.capture_chunk_samples));

  // Compute max chunks per tick (allow some slack for buffering)
  const std::size_t nominal_chunks_per_tick =
      std::max<std::size_t>(1, capture_chunk_samples_ / kws_chunk_samples_);
  max_kws_chunks_per_tick_ = nominal_chunks_per_tick * 8;

  // Initialize KWS state in context (ensure fields are set)
  auto& kws_state = context.kws_state;
  kws_state.preroll_capacity_samples_ = static_cast<std::size_t>(
      std::max(0, kws_config.preroll_ms) * std::max(1, audio_config.sample_rate) / 1000);
  kws_state.pending_max_age_chunks_ =
      std::max(1, (std::max(200, kws_config.preroll_ms) / 20));  // Tick is 20ms.

  // Clear any existing state
  kws_state.preroll_samples.clear();
  kws_state.prev_vad1_kws_gate_open = false;
  kws_state.fired_in_current_gate = false;
  kws_state.pending_hit = false;
  kws_state.pending_keyword.clear();
  kws_state.pending_json.clear();
  kws_state.pending_age_chunks = 0;
  kws_state.empty_read_ticks = 0;
  kws_state.last_wake_keyword.clear();
  kws_state.last_wake_ack_text.clear();
  kws_state.last_wake_ack_preset_file.clear();

  // Pre-allocate buffer
  kws_chunk_buffer_.resize(kws_chunk_samples_, 0.0F);

  GetLogger()->info(
      "{}: Initialized: chunk_samples={} preroll_ms={} preroll_capacity_samples={}",
      kLogTag, kws_chunk_samples_, kws_config.preroll_ms,
      kws_state.preroll_capacity_samples_);

  return Status::Ok();
}

void KwsStage::OnDetach(SessionContext& context) {
  reader_.reset();
  kws_chunk_buffer_.clear();
}

bool KwsStage::CanProcess(SessionState state) const {
  // KWS stage is always active; it monitors for wake words regardless of session state.
  // However, it respects the VAD1 gate (except for bypass in idle state).
  return true;
}

Status KwsStage::Process(SessionContext& context) {
  if (!context.config.kws.enabled || context.kws == nullptr) {
    return Status::Ok();
  }

  auto& kws_state = context.kws_state;
  const bool gate_open = context.vad1_state.kws_gate_open;
  const bool gate_just_opened = gate_open && !kws_state.prev_vad1_kws_gate_open;
  const bool gate_just_closed = !gate_open && kws_state.prev_vad1_kws_gate_open;

  // Handle gate transitions
  if (gate_just_opened) {
    Status reset_st = context.kws->Reset();
    if (!reset_st.ok()) {
      GetLogger()->warn("{}: KWS reset failed on gate open: {}", kLogTag, reset_st.message());
    } else {
      GetLogger()->debug("{}: KWS stream reset on gate open", kLogTag);
    }
    kws_state.fired_in_current_gate = false;

    // If there was a pending hit from before the gate opened, fire it now
    if (kws_state.pending_hit && !kws_state.fired_in_current_gate) {
      ++context.stats.kws_hit_count;
      kws_state.fired_in_current_gate = true;
      HandleKwsHit(context, kws_state.pending_keyword, kws_state.pending_json);
      kws_state.pending_hit = false;
      kws_state.pending_keyword.clear();
      kws_state.pending_json.clear();
      kws_state.pending_age_chunks = 0;
    }
  }

  if (gate_just_closed) {
    Status reset_st = context.kws->Reset();
    if (!reset_st.ok()) {
      GetLogger()->warn("{}: KWS reset failed on gate close: {}", kLogTag, reset_st.message());
    } else {
      GetLogger()->debug("{}: KWS stream reset on gate close", kLogTag);
    }
  }

  // Process audio chunks
  std::size_t processed_chunks = 0;
  while (processed_chunks < max_kws_chunks_per_tick_ &&
         reader_->Has(kws_chunk_buffer_.size()) &&
         reader_->ReadAndAdvance(kws_chunk_buffer_.data(),
                                 kws_chunk_buffer_.size())) {
    const ChunkStats chunk_stats = ComputeChunkStats(kws_chunk_buffer_);
    context.stats.rms_acc += chunk_stats.rms;
    context.stats.peak_acc += chunk_stats.peak;
    ++context.stats.chunk_ticks;

    // Periodic logging
    if ((context.stats.chunk_ticks % 50U) == 0U) {
      GetLogger()->debug("{}: input rms={:.4f} peak={:.4f} gate={} state={}",
                         kLogTag, chunk_stats.rms, chunk_stats.peak,
                         gate_open ? "open" : "closed",
                         static_cast<int>(context.state));
    }

    // Update preroll buffer
    if (kws_state.preroll_capacity_samples_ > 0U) {
      for (float s : kws_chunk_buffer_) {
        kws_state.preroll_samples.push_back(s);
        if (kws_state.preroll_samples.size() > kws_state.preroll_capacity_samples_) {
          kws_state.preroll_samples.pop_front();
        }
      }
    }

    // Age pending hit
    if (kws_state.pending_hit) {
      ++kws_state.pending_age_chunks;
      if (kws_state.pending_age_chunks > kws_state.pending_max_age_chunks) {
        kws_state.pending_hit = false;
        kws_state.pending_keyword.clear();
        kws_state.pending_json.clear();
        kws_state.pending_age_chunks = 0;
      }
    }

    // Run KWS engine
    KwsResult kws_result;
    Status st = context.kws->Process(kws_chunk_buffer_.data(),
                                     kws_chunk_buffer_.size(),
                                     &kws_result);
    if (!st.ok()) {
      GetLogger()->error("{}: KWS process failed: {}", kLogTag, st.message());
    } else if (kws_result.detected) {
      GetLogger()->debug("{}: detected keyword='{}' gate={} state={}",
                         kLogTag, kws_result.keyword,
                         gate_open ? "open" : "closed",
                         static_cast<int>(context.state));

      // Bypass VAD-1 gate: wake immediately on KWS hit while waiting for wakeup.
      if (context.state == SessionState::kIdle) {
        ++context.stats.kws_hit_count;
        kws_state.fired_in_current_gate = true;
        kws_state.pending_hit = false;
        kws_state.pending_keyword.clear();
        kws_state.pending_json.clear();
        kws_state.pending_age_chunks = 0;
        HandleKwsHit(context, kws_result.keyword, kws_result.json);
      } else {
        GetLogger()->debug("{}: KWS hit ignored: state={} (not idle)",
                           kLogTag, static_cast<int>(context.state));
      }
    }

    ++processed_chunks;
  }

  // Log if no chunks were processed
  if (processed_chunks == 0U) {
    ++kws_state.empty_read_ticks;
    if ((kws_state.empty_read_ticks % 50U) == 0U) {
      GetLogger()->debug("{}: idle: no chunks (kws_pos={} write_pos={} oldest_pos={})",
                         kLogTag, reader_->pos(),
                         context.ring_buffer->write_pos(),
                         context.ring_buffer->OldestPos());
    }
  } else {
    kws_state.empty_read_ticks = 0;
  }

  // Remember current gate state for next tick
  kws_state.prev_vad1_kws_gate_open = gate_open;

  return Status::Ok();
}

void KwsStage::HandleKwsHit(SessionContext& context,
                            const std::string& keyword,
                            const std::string& detail_json) {
  // Update session state
  context.state = SessionState::kWakeCandidate;

  // Store wake keyword and acknowledgment details
  auto& kws_state = context.kws_state;
  kws_state.last_wake_keyword = keyword;
  kws_state.last_wake_ack_text = ResolveWakeAckText(context.config, keyword);
  kws_state.last_wake_ack_preset_file =
      ResolveWakeAckPresetFile(context.config, keyword);

  // Schedule TTS task for wake acknowledgment
  context.tts_state.tasks.push_back(
      TtsTask{kws_state.last_wake_ack_preset_file, kws_state.last_wake_ack_text});
  context.tts_state.wake_ack_pending = true;

  GetLogger()->info("{}: wakeup hit: {}", kLogTag, keyword);
  if (!detail_json.empty()) {
    GetLogger()->debug("{}: KWS detail: {}", kLogTag, detail_json);
  }

  // Transition to pre‑listening state
  context.state = SessionState::kPreListening;
  GetLogger()->info("{}: wait for instruction", kLogTag);
}

}  // namespace mos::vis

#pragma once
#include <deque>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/asr/asr_engine.h"
#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"
#include "mos/vis/kws/kws_engine.h"
#include "mos/vis/runtime/session_state.h"
#include "mos/vis/tts/tts_engine.h"
#include "mos/vis/vad/vad_engine.h"

namespace mos::vis {

class SessionController {
 public:
  enum class Vad1Stage {
    kClosed = 0,
    kOpen,
    kHangover,
  };
  enum class Vad2Stage {
    kSilence = 0,
    kSpeech,
  };

  SessionController(const AppConfig& config, std::shared_ptr<AudioRingBuffer> ring);

  Status Initialize();
  void Tick();
  SessionState state() const;

 private:
  struct TickStats {
    std::uint64_t chunk_ticks = 0;
    std::uint64_t vad_ticks = 0;
    double rms_acc = 0.0;
    double peak_acc = 0.0;
    double vad_prob_acc = 0.0;
    std::uint64_t vad_gate_open_count = 0;
    std::uint64_t raw_speech_count = 0;
    std::uint64_t kws_hit_count = 0;
    std::uint64_t last_chunk_log_tick = 0;
  };

  struct TtsTask {
    std::string preset_file;
    std::string reply_text;
  };

  void ProcessVad1Stage(TickStats* stats);
  void ProcessKwsStage(TickStats* stats);
  void ProcessVad2Stage();
  void ProcessAsrStage();
  void ProcessTtsStage();
  void EmitFrontendStats(TickStats* stats);

  void InitializeVad1StateMachine();
  bool UpdateVad1StateMachine(float vad_probability);
  void InitializeVad2StateMachine();
  bool UpdateVad2StateMachine(float vad_probability);
  void OnVad2SpeechStart();
  void OnVad2SpeechEnd();
  std::string ResolveWakeAckText(const std::string& keyword) const;
  std::string ResolveWakeAckPresetFile(const std::string& keyword) const;
  void HandleKwsHit(const std::string& keyword, const std::string& detail_json);

  AppConfig config_;
  std::shared_ptr<AudioRingBuffer> ring_;
  AudioReader vad1_reader_;
  AudioReader kws_reader_;
  AudioReader vad2_reader_;
  AudioReader asr_reader_;
  SessionState state_ = SessionState::kIdle;

  std::unique_ptr<VadEngine> vad1_;
  std::unique_ptr<VadEngine> vad2_;
  std::unique_ptr<KwsEngine> kws_;
  std::unique_ptr<AsrEngine> asr_;
  std::unique_ptr<TtsEngine> tts_;
  TickStats stats_;
  std::deque<float> kws_preroll_samples_;
  std::size_t kws_preroll_capacity_samples_ = 0;
  bool prev_vad1_kws_gate_open_ = false;
  bool kws_fired_in_current_gate_ = false;
  bool kws_pending_hit_ = false;
  std::string kws_pending_keyword_;
  std::string kws_pending_json_;
  int kws_pending_age_chunks_ = 0;
  int kws_pending_max_age_chunks_ = 50;
  std::string last_wake_keyword_;
  std::string last_wake_ack_text_;
  std::string last_wake_ack_preset_file_;
  std::deque<TtsTask> tts_tasks_;
  bool tts_task_running_ = false;
  bool wake_ack_pending_ = false;

  Vad1Stage vad1_stage_ = Vad1Stage::kClosed;
  Vad2Stage vad2_stage_ = Vad2Stage::kSilence;
  bool vad1_kws_gate_open_ = false;
  int vad1_open_count_ = 0;
  int vad1_close_count_ = 0;
  int vad1_hangover_left_ = 0;
  int vad1_hangover_frames_ = 1;
  int vad1_min_open_frames_ = 3;
  int vad1_min_closed_frames_ = 2;
  int vad1_hangover_reopen_frames_ = 2;
  int vad1_stage_age_frames_ = 0;
  int vad1_hangover_reopen_count_ = 0;
  int vad1_instant_open_count_ = 0;
  int vad1_instant_open_frames_ = 2;
  int vad1_reopen_cooldown_frames_ = 8;
  int vad1_reopen_cooldown_left_ = 0;
  float vad1_prob_ema_ = 0.0F;
  float vad1_ema_alpha_ = 0.45F;
  float vad1_open_threshold_ = 0.35F;
  float vad1_close_threshold_ = 0.20F;
  float vad1_instant_open_threshold_ = 0.55F;

  int vad2_start_count_ = 0;
  int vad2_end_count_ = 0;
  int vad2_hangover_left_ = 0;
  int vad2_hangover_frames_ = 1;
  float vad2_prob_ema_ = 0.0F;
  float vad2_ema_alpha_ = 0.35F;
  float vad2_start_threshold_ = 0.50F;
  float vad2_end_threshold_ = 0.25F;

  std::size_t asr_processed_samples_ = 0;
  std::string last_partial_asr_text_;
  std::optional<std::uint64_t> wake_pos_samples_;
};

}  // namespace mos::vis

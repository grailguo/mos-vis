#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/asr/asr_engine.h"
#include "mos/vis/config/app_config.h"
#include "mos/vis/nlu/nlu_engine.h"
#include "mos/vis/runtime/subsm/hotspot_subsms.h"
#include "mos/vis/runtime/session_state.h"
// StateMachineController for v3 architecture
#include "mos/vis/runtime/state_machine_controller.h"

// Forward declarations for engine interfaces
namespace mos::vis {
class VadEngine;
class KwsEngine;
class AsrEngine;
class NluEngine;
class ControlEngine;
class TtsEngine;

// TTS task definition
struct TtsTask {
  std::string preset_file;
  std::string reply_text;
};

/**
 * @brief Shared context for all pipeline stages.
 *
 * SessionContext contains all shared state, engine instances, and configuration
 * that pipeline stages need to access. It serves as the communication mechanism
 * between stages while maintaining encapsulation.
 */
struct SessionContext {
  // === Immutable Configuration ===
  const AppConfig& config;

  // === Shared Engine Instances ===
  std::shared_ptr<AudioRingBuffer> ring_buffer;
  std::shared_ptr<VadEngine> vad1;
  std::shared_ptr<VadEngine> vad2;
  std::shared_ptr<KwsEngine> kws;
  std::shared_ptr<AsrEngine> asr;
  std::shared_ptr<NluEngine> nlu;
  std::shared_ptr<ControlEngine> control;
  std::shared_ptr<TtsEngine> tts;

  // === Session State ===
  SessionState state = SessionState::kIdle;
  std::string session_id;
  std::uint64_t turn_id = 0;
  bool keep_session_open = true;
  std::string current_control_request_id;
  std::uint64_t reply_playback_token = 0;
  bool reply_tts_started = false;
  bool barge_in_enabled = true;
  std::chrono::steady_clock::time_point last_wake_timestamp{};

  // === Audio Reader Positions ===
  // Each stage maintains its own reader, but we track positions here
  // for coordination and debugging
  struct ReaderPositions {
    std::uint64_t vad1_pos = 0;
    std::uint64_t kws_pos = 0;
    std::uint64_t vad2_pos = 0;
    std::uint64_t asr_pos = 0;
  } reader_pos;

  // === VAD1 State Machine State ===
  enum class Vad1MachineStage {
    kClosed = 0,
    kOpen,
    kHangover,
  };

  struct Vad1State {
    Vad1MachineStage stage = Vad1MachineStage::kClosed;
    bool kws_gate_open = false;
    int open_count = 0;
    int close_count = 0;
    int hangover_left = 0;
    int hangover_frames = 1;
    int min_open_frames = 3;
    int min_closed_frames = 2;
    int hangover_reopen_frames = 2;
    int stage_age_frames = 0;
    int hangover_reopen_count = 0;
    int instant_open_count = 0;
    int instant_open_frames = 2;
    int reopen_cooldown_frames = 8;
    int reopen_cooldown_left = 0;
    float prob_ema = 0.0F;
    float ema_alpha = 0.45F;
    float open_threshold = 0.35F;
    float close_threshold = 0.20F;
    float instant_open_threshold = 0.55F;
  } vad1_state;

  // === VAD2 State Machine State ===
  enum class Vad2MachineStage {
    kSilence = 0,
    kSpeech,
  };

  struct Vad2State {
    Vad2MachineStage stage = Vad2MachineStage::kSilence;
    int start_count = 0;
    int end_count = 0;
    int hangover_left = 0;
    int hangover_frames = 1;
    float prob_ema = 0.0F;
    float ema_alpha = 0.35F;
    float start_threshold = 0.50F;
    float end_threshold = 0.25F;
    std::chrono::steady_clock::time_point speech_start_time;
    int min_speech_duration_ms = 500;  // Minimum speech duration to consider valid
  } vad2_state;

  // === KWS State ===
  struct KwsState {
    std::deque<float> preroll_samples;
    std::size_t preroll_capacity_samples = 0;
    bool prev_vad1_kws_gate_open = false;
    bool fired_in_current_gate = false;
    bool pending_hit = false;
    std::string pending_keyword;
    std::string pending_json;
    int pending_age_chunks = 0;
    int pending_max_age_chunks = 50;
    std::uint64_t empty_read_ticks = 0;
    std::string last_wake_keyword;
    std::string last_wake_ack_text;
    std::string last_wake_ack_preset_file;
  } kws_state;

  // === ASR State ===
  struct AsrState {
    std::size_t processed_samples = 0;
    std::string last_partial_text;
    std::chrono::steady_clock::time_point last_text_update_time;
    int no_text_timeout_seconds = 15;
    bool has_pending_final_result = false;
    bool should_finalize = false;  // Flag set by state machine to trigger ASR finalization
    // Note: pending_asr_final_result_ and pending_nlu_result_ are stored separately
  } asr_state;

  // === TTS State ===
  struct TtsState {
    std::deque<TtsTask> tasks;
    bool task_running = false;
    bool wake_ack_pending = false;
  } tts_state;

  // === Local event bus for Layer-2 hotspot sub-state machines ===
  subsm::LocalEventBus local_events;

  // === V3 State Machine Controller ===
  std::unique_ptr<StateMachineController> state_machine;

  struct SubsmState {
    subsm::WakeState wake = subsm::WakeState::kWaitingWakeup;
    subsm::AsrState asr = subsm::AsrState::kListening;
    subsm::ReplyState reply = subsm::ReplyState::kResultSpeaking;
    int wake_cooldown_ms = 2000;
  } subsm_state;

  // === NLU & Control State ===
  struct NluControlState {
    bool has_pending_nlu_result = false;
    std::optional<NluResult> pending_nlu_result;
    bool has_pending_asr_final_result = false;
    std::optional<AsrResult> pending_asr_final_result;
    std::string pending_control_text;
    std::optional<std::uint64_t> wake_pos_samples;
  } nlu_control_state;

  // === Statistics ===
  struct Statistics {
    std::uint64_t chunk_ticks = 0;
    std::uint64_t vad_ticks = 0;
    double rms_acc = 0.0;
    double peak_acc = 0.0;
    double vad_prob_acc = 0.0;
    std::uint64_t vad_gate_open_count = 0;
    std::uint64_t raw_speech_count = 0;
    std::uint64_t kws_hit_count = 0;
    std::uint64_t last_chunk_log_tick = 0;
  } stats;

  // === Constructor ===
  explicit SessionContext(const AppConfig& config_ref)
      : config(config_ref) {}

  // Prevent copying
  SessionContext(const SessionContext&) = delete;
  SessionContext& operator=(const SessionContext&) = delete;

  // Allow moving
  SessionContext(SessionContext&&) = default;
  SessionContext& operator=(SessionContext&&) = default;
};

}  // namespace mos::vis

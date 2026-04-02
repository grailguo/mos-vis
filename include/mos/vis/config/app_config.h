#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mos::vis {

struct AgentConfig {
  bool enable_metrics = true;
  std::uint32_t asr_listen_timeout_ms = 15000;
};

struct AudioConfig {
  std::uint32_t sample_rate_hz = 16000;
  std::string input_device;
  std::string output_device;
  std::uint32_t audio_channels = 1;
};

struct VadConfig {
  float threshold = 0.55f;
  std::uint32_t min_speech_ms = 180;
  std::uint32_t min_silence_ms = 300;
  bool enable_barge_in = true;
  std::string backend = "rknn";
  std::string model_path;
};

struct KwsConfig {
  std::string keyword = "mos";
  float trigger_threshold = 0.70f;
  std::uint32_t pre_roll_ms = 600;
  std::string backend = "sherpa-onnx";
  std::string model_path;
};

struct AsrConfig {
  std::string model_path;
  std::uint32_t partial_interval_ms = 200;
  std::string language = "zh-en";
  std::string backend = "sherpa-onnx";
};

struct NluConfig {
  bool critical_confirmation_required = true;
};

struct ControlConfig {
  std::string url = "ws://127.0.0.1:9000/ws";
  std::uint32_t timeout_ms = 1500;
  std::uint32_t retry_count = 1;
  std::string request_ack_phrase;
  bool simulation_mode = false;
};

struct TtsConfig {
  std::string model_path;
  float speed = 1.0f;
  std::string barge_in_mode = "vad-only";
  std::string backend = "sherpa-onnx";
};

struct LoggingConfig {
  std::string level = "info";
  bool structured = false;
};

struct RagConfig {
  bool enabled = false;
  std::uint32_t top_k = 4;
  float score_threshold = 0.35f;
  std::uint32_t max_context_tokens = 1200;
  std::uint32_t request_timeout_ms = 800;
  std::string index_path;
  std::uint32_t cache_ttl_s = 60;
};

struct RingBufferConfig {
  std::uint32_t frame_capacity = 8192;
  std::uint32_t sample_capacity = 8192;
};

struct WakeAckText {
  std::vector<std::string> keywords;
  std::string reply_text;
  std::string preset_file;
};

struct AppConfig {
  AgentConfig agent;
  AudioConfig audio;
  VadConfig vad;
  KwsConfig kws;
  AsrConfig asr;
  NluConfig nlu;
  ControlConfig control;
  TtsConfig tts;
  LoggingConfig logging;
  RagConfig rag;
  RingBufferConfig ring_buffer;
  std::vector<WakeAckText> wake_ack_text;

  // Compatibility fields for existing call-sites.
  std::string log_level = "info";
  std::string control_url = "ws://127.0.0.1:9000/ws";
  std::string device_id = "mos-device";
  std::uint32_t sample_rate_hz = 16000;
  std::uint32_t audio_channels = 1;
  std::uint32_t ring_buffer_capacity = 8192;
};

}  // namespace mos::vis

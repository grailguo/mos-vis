#include "mos/vis/config/config_loader.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace mos::vis {

namespace {

constexpr const char* kDebugDefaultConfigPath = "./config/config.json";
constexpr const char* kReleaseDefaultConfigPath = "/etc/mos_vis/config.json";
constexpr const char* kDebugFallbackConfigPath1 = "../config/config.json";
constexpr const char* kDebugFallbackConfigPath2 = "../../config/config.json";

std::string ResolveDebugConfigPath() {
  const std::filesystem::path candidates[] = {
      kDebugDefaultConfigPath,
      kDebugFallbackConfigPath1,
      kDebugFallbackConfigPath2,
  };

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }
  return kDebugDefaultConfigPath;
}

void ValidateConfigOrThrow(const AppConfig& config) {
  const std::uint64_t required_pre_roll_frames =
      (static_cast<std::uint64_t>(config.audio.sample_rate_hz) * config.kws.pre_roll_ms) / 1000ULL;
  if (static_cast<std::uint64_t>(config.ring_buffer.frame_capacity) < required_pre_roll_frames) {
    throw std::runtime_error(
        "ring_buffer_capacity is smaller than required KWS pre-roll frames");
  }
}

}  // namespace

AppConfig ConfigLoader::LoadFromFile(const std::string& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open config: " + path);
  }

  nlohmann::json json;
  input >> json;

  AppConfig config;

  if (json.contains("agent") && json["agent"].is_object()) {
    const auto& agent = json["agent"];
    config.agent.enable_metrics = agent.value("enable_metrics", config.agent.enable_metrics);
    config.agent.asr_listen_timeout_ms =
        agent.value("asr_listen_timeout_ms", config.agent.asr_listen_timeout_ms);
  }

  if (json.contains("audio") && json["audio"].is_object()) {
    const auto& audio = json["audio"];
    config.audio.sample_rate_hz = audio.value("sample_rate_hz", config.audio.sample_rate_hz);
    config.audio.input_device = audio.value("input_device", config.audio.input_device);
    config.audio.output_device = audio.value("output_device", config.audio.output_device);
    config.audio.audio_channels = audio.value("audio_channels", config.audio.audio_channels);
  }

  if (json.contains("vad") && json["vad"].is_object()) {
    const auto& vad = json["vad"];
    config.vad.threshold = vad.value("threshold", config.vad.threshold);
    config.vad.min_speech_ms = vad.value("min_speech_ms", config.vad.min_speech_ms);
    config.vad.min_silence_ms = vad.value("min_silence_ms", config.vad.min_silence_ms);
    config.vad.enable_barge_in = vad.value("enable_barge_in", config.vad.enable_barge_in);
    config.vad.backend = vad.value("backend", config.vad.backend);
    config.vad.model_path = vad.value("model_path", config.vad.model_path);
  }

  if (json.contains("kws") && json["kws"].is_object()) {
    const auto& kws = json["kws"];
    config.kws.keyword = kws.value("keyword", config.kws.keyword);
    config.kws.trigger_threshold = kws.value("trigger_threshold", config.kws.trigger_threshold);
    config.kws.pre_roll_ms = kws.value("pre_roll_ms", config.kws.pre_roll_ms);
    config.kws.backend = kws.value("backend", config.kws.backend);
    config.kws.model_path = kws.value("model_path", config.kws.model_path);
  }

  if (json.contains("asr") && json["asr"].is_object()) {
    const auto& asr = json["asr"];
    config.asr.model_path = asr.value("model_path", config.asr.model_path);
    config.asr.partial_interval_ms = asr.value("partial_interval_ms", config.asr.partial_interval_ms);
    config.asr.language = asr.value("language", config.asr.language);
    config.asr.backend = asr.value("backend", config.asr.backend);
  }

  if (json.contains("nlu") && json["nlu"].is_object()) {
    const auto& nlu = json["nlu"];
    config.nlu.critical_confirmation_required =
        nlu.value("critical_confirmation_required", config.nlu.critical_confirmation_required);
  }

  if (json.contains("control") && json["control"].is_object()) {
    const auto& control = json["control"];
    config.control.url = control.value("url", config.control.url);
    config.control.timeout_ms = control.value("timeout_ms", config.control.timeout_ms);
    config.control.retry_count = control.value("retry_count", config.control.retry_count);
    config.control.request_ack_phrase =
        control.value("request_ack_phrase", config.control.request_ack_phrase);
  }

  if (json.contains("tts") && json["tts"].is_object()) {
    const auto& tts = json["tts"];
    config.tts.model_path = tts.value("model_path", config.tts.model_path);
    config.tts.speed = tts.value("speed", config.tts.speed);
    config.tts.barge_in_mode = tts.value("barge_in_mode", config.tts.barge_in_mode);
    config.tts.backend = tts.value("backend", config.tts.backend);
  }

  if (json.contains("logging") && json["logging"].is_object()) {
    const auto& logging = json["logging"];
    config.logging.level = logging.value("level", config.logging.level);
    config.logging.structured = logging.value("structured", config.logging.structured);
  }

  if (json.contains("rag") && json["rag"].is_object()) {
    const auto& rag = json["rag"];
    config.rag.enabled = rag.value("enabled", config.rag.enabled);
    config.rag.top_k = rag.value("top_k", config.rag.top_k);
    config.rag.score_threshold = rag.value("score_threshold", config.rag.score_threshold);
    config.rag.max_context_tokens = rag.value("max_context_tokens", config.rag.max_context_tokens);
    config.rag.request_timeout_ms = rag.value("request_timeout_ms", config.rag.request_timeout_ms);
    config.rag.index_path = rag.value("index_path", config.rag.index_path);
    config.rag.cache_ttl_s = rag.value("cache_ttl_s", config.rag.cache_ttl_s);
  }

  bool has_ring_buffer_object = false;
  if (json.contains("ring_buffer") && json["ring_buffer"].is_object()) {
    has_ring_buffer_object = true;
    const auto& ring_buffer = json["ring_buffer"];
    config.ring_buffer.frame_capacity =
        ring_buffer.value("frame_capacity", config.ring_buffer.frame_capacity);
    config.ring_buffer.sample_capacity =
        ring_buffer.value("sample_capacity", config.ring_buffer.sample_capacity);
  }

  if (json.contains("wake_ack_text") && json["wake_ack_text"].is_array()) {
    for (const auto& item : json["wake_ack_text"]) {
      if (!item.is_object()) {
        continue;
      }
      WakeAckText entry;
      if (item.contains("keywords") && item["keywords"].is_array()) {
        for (const auto& keyword : item["keywords"]) {
          if (keyword.is_string()) {
            entry.keywords.push_back(keyword.get<std::string>());
          }
        }
      }
      entry.reply_text = item.value("reply_text", entry.reply_text);
      entry.preset_file = item.value("preset_file", entry.preset_file);
      config.wake_ack_text.push_back(std::move(entry));
    }
  }

  // Backward-compatible top-level keys.
  config.logging.level = json.value("log_level", config.logging.level);
  config.control.url = json.value("control_url", config.control.url);
  config.device_id = json.value("device_id", config.device_id);
  config.audio.sample_rate_hz = json.value("sample_rate_hz", config.audio.sample_rate_hz);
  config.audio.audio_channels = json.value("audio_channels", config.audio.audio_channels);
  config.ring_buffer_capacity =
      json.value("ring_buffer_capacity", config.ring_buffer_capacity);

  if (has_ring_buffer_object) {
    // If object form is present, use it as source of truth.
    config.ring_buffer_capacity = config.ring_buffer.frame_capacity;
  } else {
    // Keep compatibility with legacy top-level key.
    config.ring_buffer.frame_capacity = config.ring_buffer_capacity;
    config.ring_buffer.sample_capacity = config.ring_buffer_capacity;
  }

  // Compatibility mirrors for existing call-sites.
  config.log_level = config.logging.level;
  config.control_url = config.control.url;
  config.sample_rate_hz = config.audio.sample_rate_hz;
  config.audio_channels = config.audio.audio_channels;

  ValidateConfigOrThrow(config);
  return config;
}

std::string ResolveConfigPath(int argc, char** argv, bool is_release_build) {
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--config") {
      return argv[i + 1];
    }
  }
  return is_release_build ? kReleaseDefaultConfigPath : ResolveDebugConfigPath();
}

}  // namespace mos::vis

#include "mos/vis/config/app_config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "mos/vis/common/logging.h"

namespace mos::vis {
namespace {
using json = nlohmann::json;

constexpr float kGainMin = 0.1F;
constexpr float kGainMax = 4.0F;
constexpr float kRecommendedCombinedGainMax = 3.0F;

std::string ResolvePathFromConfigDir(const std::filesystem::path& config_dir,
                                     const std::string& value) {
  if (value.empty()) {
    return value;
  }
  const std::filesystem::path p(value);
  if (p.is_absolute()) {
    return value;
  }

  // Keep existing behavior first: relative to current working directory.
  if (std::filesystem::exists(p)) {
    return p.lexically_normal().string();
  }

  // Then try relative to the config directory itself.
  const std::filesystem::path from_config_dir = (config_dir / p).lexically_normal();
  if (std::filesystem::exists(from_config_dir)) {
    return from_config_dir.string();
  }

  // Finally, support layouts like repo/config/config.json with models under repo/models.
  const std::filesystem::path from_config_parent =
      (config_dir.parent_path() / p).lexically_normal();
  if (std::filesystem::exists(from_config_parent)) {
    return from_config_parent.string();
  }

  return p.lexically_normal().string();
}

std::string JsonFieldToString(const json& node) {
  if (node.is_string()) {
    return node.get<std::string>();
  }
  return node.dump();
}

float ClampGainWithWarning(float value, const char* key) {
  float clamped = value;
  if (clamped < kGainMin) {
    clamped = kGainMin;
  } else if (clamped > kGainMax) {
    clamped = kGainMax;
  }
  if (clamped != value) {
    LogWarn(logevent::kSystemBoot, LogContext{"AppConfig", "", 0, "", ""},
            {Kv("detail", "config_value_clamped"),
             Kv("key", key),
             Kv("input", value),
             Kv("clamped", clamped),
             Kv("range", "0.1~4.0")});
  }
  return clamped;
}

std::string NormalizeTtsEngineId(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
}

Status AppConfig::LoadFromFile(const std::string& path, AppConfig* config) {
  if (config == nullptr) {
    return Status::InvalidArgument("config pointer is null");
  }

  std::ifstream ifs(path);
  if (!ifs) {
    return Status::NotFound("failed to open config file: " + path);
  }
  const std::filesystem::path config_dir = std::filesystem::path(path).parent_path();

  json j;
  try {
    ifs >> j;
    const auto& a = j.at("audio");
    config->audio.sample_rate = a.value("sample_rate", 16000);
    config->audio.channels = a.value("channels", 1);
    config->audio.capture_chunk_samples = a.value("capture_chunk_samples", 320);
    config->audio.ring_seconds = a.value("ring_seconds", 10);
    config->audio.playback_gain = a.value("playback_gain", config->audio.playback_gain);
    config->audio.input_device = a.value("input_device", "");
    config->audio.output_device = a.value("output_device", "");
    config->audio.channel_select_mode = a.value("channel_select_mode", "auto_track");
    config->audio.fixed_channel_index = a.value("fixed_channel_index", 0);
    config->audio.track_switch_consecutive = a.value("track_switch_consecutive", 3);

    const auto& v1 = j.at("vad1");
    config->vad1.enabled = v1.value("enabled", true);
    config->vad1.model_path = ResolvePathFromConfigDir(config_dir, v1.value("model_path", ""));
    config->vad1.threshold = v1.value("threshold", 0.35F);
    config->vad1.window_samples = v1.value("window_samples", 512);
    config->vad1.hop_samples = v1.value("hop_samples", 160);
    config->vad1.open_frames = v1.value("open_frames", 2);
    config->vad1.close_frames = v1.value("close_frames", 10);
    config->vad1.hangover_ms = v1.value("hangover_ms", 160);

    const auto& kws = j.at("kws");
    config->kws.enabled = kws.value("enabled", true);
    config->kws.model_dir = ResolvePathFromConfigDir(config_dir, kws.value("model_dir", ""));
    config->kws.chunk_samples = kws.value("chunk_samples", 320);
    config->kws.preroll_ms = kws.value("preroll_ms", 400);

    const auto& v2 = j.at("vad2");
    config->vad2.enabled = v2.value("enabled", true);
    config->vad2.model_path = ResolvePathFromConfigDir(config_dir, v2.value("model_path", ""));
    config->vad2.threshold = v2.value("threshold", 0.50F);
    config->vad2.window_samples = v2.value("window_samples", 512);
    config->vad2.hop_samples = v2.value("hop_samples", 160);
    config->vad2.start_frames = v2.value("start_frames", 3);
    config->vad2.end_frames = v2.value("end_frames", 20);
    config->vad2.hangover_ms = v2.value("hangover_ms", 320);

    const auto& asr = j.at("asr");
    config->asr.enabled = asr.value("enabled", true);
    config->asr.model_dir = ResolvePathFromConfigDir(config_dir, asr.value("model_dir", ""));
    config->asr.chunk_samples = asr.value("chunk_samples", 320);
    config->asr.preroll_ms = asr.value("preroll_ms", 400);
    config->asr.tail_ms = asr.value("tail_ms", 240);

    const auto& tts = j.at("tts");
    config->tts.enabled = tts.value("enabled", true);
    if (!tts.contains("engine") || !tts.at("engine").is_string() ||
        tts.at("engine").get<std::string>().empty()) {
      return Status::InvalidArgument("tts.engine is required and must be a non-empty string");
    }
    config->tts.engine = NormalizeTtsEngineId(tts.at("engine").get<std::string>());
    if (config->tts.engine != "sherpa" && config->tts.engine != "vits_melo") {
      return Status::InvalidArgument(
          "unsupported tts.engine: " + config->tts.engine +
          " (supported: sherpa, vits_melo)");
    }

    if (tts.contains("common") && tts.at("common").is_object()) {
      const auto& common = tts.at("common");
      config->tts.fixed_phrase_cache =
          common.value("fixed_phrase_cache", config->tts.fixed_phrase_cache);
      config->tts.volume_gain = common.value("volume_gain", config->tts.volume_gain);
    } else {
      config->tts.fixed_phrase_cache = tts.value("fixed_phrase_cache", true);
      config->tts.volume_gain = tts.value("volume_gain", config->tts.volume_gain);
    }

    const bool has_engines = tts.contains("engines") && tts.at("engines").is_object();
    const bool has_engine_node =
        has_engines && tts.at("engines").contains(config->tts.engine) &&
        tts.at("engines").at(config->tts.engine).is_object();
    if (has_engine_node) {
      const auto& selected = tts.at("engines").at(config->tts.engine);
      config->tts.model_dir =
          ResolvePathFromConfigDir(config_dir, selected.value("model_dir", ""));
      config->tts.provider = selected.value("provider", config->tts.provider);
      config->tts.num_threads = selected.value("num_threads", config->tts.num_threads);
      config->tts.max_num_sentences =
          selected.value("max_num_sentences", config->tts.max_num_sentences);
      config->tts.silence_scale =
          selected.value("silence_scale", config->tts.silence_scale);
      config->tts.reference_wav =
          ResolvePathFromConfigDir(config_dir, selected.value("reference_wav", ""));
      config->tts.reference_text =
          selected.value("reference_text", config->tts.reference_text);
      if (config->tts.engine == "vits_melo") {
        config->tts.use_int8 = selected.value("use_int8", config->tts.use_int8);
      } else {
        config->tts.use_int8 = selected.value("use_int8", true);
      }
    } else {
      // Backward-compatible fallback for legacy flat keys under tts.*
      config->tts.model_dir =
          ResolvePathFromConfigDir(config_dir, tts.value("model_dir", ""));
      config->tts.use_int8 = tts.value("use_int8", config->tts.use_int8);
      config->tts.provider = tts.value("provider", config->tts.provider);
      config->tts.num_threads = tts.value("num_threads", config->tts.num_threads);
      config->tts.max_num_sentences =
          tts.value("max_num_sentences", config->tts.max_num_sentences);
      config->tts.silence_scale = tts.value("silence_scale", config->tts.silence_scale);
      config->tts.reference_wav =
          ResolvePathFromConfigDir(config_dir, tts.value("reference_wav", ""));
      config->tts.reference_text =
          tts.value("reference_text", config->tts.reference_text);

      LogWarn(logevent::kSystemBoot, LogContext{"AppConfig", "", 0, "", ""},
              {Kv("detail", "tts_legacy_config_fallback"),
               Kv("engine", config->tts.engine),
               Kv("hint", "use tts.common + tts.engines.<engine_id> schema")});
    }

    if (config->tts.model_dir.empty()) {
      return Status::InvalidArgument(
          "tts.engines." + config->tts.engine + ".model_dir is required");
    }

    config->audio.playback_gain =
        ClampGainWithWarning(config->audio.playback_gain, "audio.playback_gain");
    config->tts.volume_gain =
        ClampGainWithWarning(config->tts.volume_gain, "tts.volume_gain");
    const float combined_gain = config->audio.playback_gain * config->tts.volume_gain;
    if (combined_gain > kRecommendedCombinedGainMax) {
      LogWarn(logevent::kSystemBoot, LogContext{"AppConfig", "", 0, "", ""},
              {Kv("detail", "combined_gain_high"),
               Kv("audio.playback_gain", config->audio.playback_gain),
               Kv("tts.volume_gain", config->tts.volume_gain),
               Kv("combined_gain", combined_gain),
               Kv("recommended_max", kRecommendedCombinedGainMax)});
    }

    if (j.contains("nlu") && j.at("nlu").is_object()) {
      const auto& nlu = j.at("nlu");
      config->nlu.enabled = nlu.value("enabled", true);
      config->nlu.model_dir = ResolvePathFromConfigDir(config_dir, nlu.value("model_dir", ""));
      config->nlu.provider = nlu.value("provider", "cpu");
      config->nlu.num_threads = nlu.value("num_threads", 1);
    } else {
      config->nlu.enabled = true;
      config->nlu.model_dir = "";
      config->nlu.provider = "cpu";
      config->nlu.num_threads = 1;
    }

    if (j.contains("control") && j.at("control").is_object()) {
      const auto& c = j.at("control");
      config->control.enabled = c.value("enabled", true);
      config->control.host = c.value("host", "127.0.0.1");
      config->control.port = c.value("port", 9000);
      config->control.path = c.value("path", "/");
      config->control.client_name = c.value("client_name", config->control.client_name);
      config->control.client_version = c.value("client_version", config->control.client_version);
      config->control.client_key = c.value("client_key", "");
      config->control.compatibility_mode = c.value("compatibility_mode", 0);
      config->control.license_user_name =
          c.value("license_user_name", config->control.license_user_name);
      config->control.version = c.value("version", config->control.version);
      config->control.authorization_timeout_sec =
          c.value("authorization_timeout_sec", config->control.authorization_timeout_sec);

      if (c.contains("start_calibration_parameter")) {
        config->control.start_calibration_parameter_json =
            JsonFieldToString(c.at("start_calibration_parameter"));
      }
      if (c.contains("start_analysis_parameter")) {
        config->control.start_analysis_parameter_json =
            JsonFieldToString(c.at("start_analysis_parameter"));
      }
    }

    config->wake_ack_text.clear();
    if (j.contains("wake_ack_text") && j.at("wake_ack_text").is_array()) {
      for (const auto& item : j.at("wake_ack_text")) {
        WakeAckRule rule;
        rule.preset_file = item.value("preset_file", "");
        rule.reply_text = item.value("reply_text", "");
        if (item.contains("keywords") && item.at("keywords").is_array()) {
          for (const auto& kw : item.at("keywords")) {
            if (kw.is_string()) {
              rule.keywords.push_back(kw.get<std::string>());
            }
          }
        }
        if (!rule.keywords.empty()) {
          config->wake_ack_text.push_back(std::move(rule));
        }
      }
    }
  } catch (const std::exception& e) {
    return Status::Internal(std::string("parse config failed: ") + e.what());
  }

  return Status::Ok();
}

}  // namespace mos::vis

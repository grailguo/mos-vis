#include "mos/vis/config/app_config.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace mos::vis {
namespace {
using json = nlohmann::json;

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
    config->tts.model_dir = ResolvePathFromConfigDir(config_dir, tts.value("model_dir", ""));
    config->tts.use_int8 = tts.value("use_int8", true);
    config->tts.fixed_phrase_cache = tts.value("fixed_phrase_cache", true);

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
      config->control.calibration_duration_sec =
          c.value("calibration_duration_sec", config->control.calibration_duration_sec);
      config->control.analysis_duration_sec =
          c.value("analysis_duration_sec", config->control.analysis_duration_sec);

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

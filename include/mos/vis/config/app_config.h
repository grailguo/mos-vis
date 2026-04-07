#pragma once
#include <string>
#include <vector>
#include "mos/vis/common/status.h"

namespace mos::vis {

struct AudioConfig {
  int sample_rate = 16000;
  int channels = 1;
  int capture_chunk_samples = 320;
  int ring_seconds = 10;
  std::string input_device;
  std::string output_device;
  std::string channel_select_mode = "auto_track";
  int fixed_channel_index = 0;
  int track_switch_consecutive = 3;
};

struct VadConfig {
  bool enabled = true;
  std::string model_path;
  float threshold = 0.50F;
  int window_samples = 512;
  int hop_samples = 160;
  int open_frames = 2;
  int close_frames = 10;
  int start_frames = 3;
  int end_frames = 20;
  int hangover_ms = 160;
};

struct KwsConfig {
  bool enabled = true;
  std::string model_dir;
  int chunk_samples = 320;
  int preroll_ms = 400;
};

struct AsrConfig {
  bool enabled = true;
  std::string model_dir;
  int chunk_samples = 320;
  int preroll_ms = 400;
  int tail_ms = 240;
};

struct TtsConfig {
  bool enabled = true;
  std::string model_dir;
  bool use_int8 = true;
  bool fixed_phrase_cache = true;
};

struct ControlConfig {
  bool enabled = true;
  std::string host = "127.0.0.1";
  int port = 9000;
  std::string path = "/";

  std::string client_name = "MOS-VIS";
  std::string client_version = "4.1.0.1";
  std::string client_key;
  int compatibility_mode = 0;
  std::string license_user_name = "MOS-VIS";
  std::string version = "3.6.2.0";

  std::string start_calibration_parameter_json;
  std::string start_analysis_parameter_json;

  int authorization_timeout_sec = 60;
  int calibration_duration_sec = 900;
  int analysis_duration_sec = 900;
};

struct WakeAckRule {
  std::vector<std::string> keywords;
  std::string preset_file;
  std::string reply_text;
};

struct AppConfig {
  AudioConfig audio;
  VadConfig vad1;
  KwsConfig kws;
  VadConfig vad2;
  AsrConfig asr;
  TtsConfig tts;
  ControlConfig control;
  std::vector<WakeAckRule> wake_ack_text;

  static Status LoadFromFile(const std::string& path, AppConfig* config);
};

}  // namespace mos::vis

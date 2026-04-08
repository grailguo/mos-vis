#include "mos/vis/tts/tts_engine.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mos/vis/audio/audio_playback.h"
#include "mos/vis/common/logging.h"

#ifdef MOS_VIS_HAS_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace mos::vis {
namespace {

#ifdef MOS_VIS_HAS_SHERPA_ONNX

class SherpaTtsEngine final : public TtsEngine {
 public:
  SherpaTtsEngine() = default;

  ~SherpaTtsEngine() override {
    if (playback_ != nullptr) {
      playback_->Stop();
    }
    if (tts_ != nullptr) {
      SherpaOnnxDestroyOfflineTts(tts_);
      tts_ = nullptr;
    }
  }

  Status Initialize(const TtsConfig& config, const AudioConfig& audio_config) override {
    config_ = config;
    const std::filesystem::path model_dir(config.model_dir);
    if (!std::filesystem::exists(model_dir)) {
      return Status::NotFound("TTS model_dir not found: " + config.model_dir);
    }

    const std::filesystem::path model_fp_path = (model_dir / "model.onnx");
    const std::filesystem::path model_int8_path = (model_dir / "model.int8.onnx");
    const std::string model_fp = model_fp_path.string();
    const std::string model_int8 = model_int8_path.string();
    const std::string tokens = (model_dir / "tokens.txt").string();
    const std::string lexicon = (model_dir / "lexicon.txt").string();
    const std::string data_dir = (model_dir / "dict").string();
    const std::filesystem::path vits_phontab_path =
        std::filesystem::path(data_dir) / "phontab";

    const bool has_fp = std::filesystem::exists(model_fp);
    const bool has_int8 = std::filesystem::exists(model_int8);
    std::error_code file_size_ec;
    const std::uintmax_t int8_size =
        has_int8 ? std::filesystem::file_size(model_int8_path, file_size_ec) : 0U;
    const bool int8_looks_valid =
        has_int8 && !file_size_ec && int8_size >= 4096U;
    const std::string model = (config.use_int8 && int8_looks_valid)
                                  ? model_int8
                                  : (has_fp ? model_fp : model_int8);
    const bool has_vits_data_dir = std::filesystem::exists(vits_phontab_path);

    if (!std::filesystem::exists(model) ||
        !std::filesystem::exists(tokens) ||
        !std::filesystem::exists(lexicon)) {
      return Status::NotFound(
          "TTS model files missing. Need model/tokens/lexicon in " +
          config.model_dir);
    }

    SherpaOnnxOfflineTtsConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.model.vits.model = model.c_str();
    cfg.model.vits.lexicon = lexicon.c_str();
    cfg.model.vits.tokens = tokens.c_str();
    cfg.model.vits.data_dir = has_vits_data_dir ? data_dir.c_str() : nullptr;
    cfg.model.vits.noise_scale = 0.667F;
    cfg.model.vits.noise_scale_w = 0.8F;
    cfg.model.vits.length_scale = 1.0F;
    cfg.model.num_threads = 1;
    cfg.model.debug = 0;
    cfg.model.provider = "cpu";
    cfg.max_num_sentences = 2;
    cfg.silence_scale = 0.2F;

    tts_ = SherpaOnnxCreateOfflineTts(&cfg);
    if (tts_ == nullptr) {
      return Status::Internal("SherpaOnnxCreateOfflineTts failed");
    }

    playback_ = CreateAudioPlayback();
    Status playback_st = playback_->Initialize(audio_config);
    if (!playback_st.ok()) {
      return playback_st;
    }
    playback_st = playback_->Start();
    if (!playback_st.ok()) {
      return playback_st;
    }

    LogInfo(logevent::kSystemBoot, LogContext{"TtsEngine", "", 0, "", ""},
            {Kv("detail", "tts_initialized"),
             Kv("model", BasenamePath(model)),
             Kv("use_int8", (model == model_int8) ? 1 : 0),
             Kv("int8_size", static_cast<unsigned long long>(int8_size)),
             Kv("data_dir", has_vits_data_dir ? BasenamePath(data_dir) : "<none>")});
    return Status::Ok();
  }

  Status Speak(const std::string& text) override {
    if (text.empty()) {
      return Status::Ok();
    }
    if (tts_ == nullptr) {
      return Status::Internal("TTS not initialized");
    }
    CachedAudio generated_audio;
    const CachedAudio* audio_for_playback = nullptr;
    if (config_.fixed_phrase_cache) {
      auto it = fixed_phrase_cache_.find(text);
      if (it != fixed_phrase_cache_.end()) {
        audio_for_playback = &it->second;
      }
    }
    if (audio_for_playback == nullptr) {
      Status gen_st = GenerateAudio(text, &generated_audio);
      if (!gen_st.ok()) {
        return gen_st;
      }
      if (config_.fixed_phrase_cache) {
        fixed_phrase_cache_[text] = generated_audio;
        audio_for_playback = &fixed_phrase_cache_[text];
      } else {
        audio_for_playback = &generated_audio;
      }
    }

    if (playback_ == nullptr) {
      return Status::Internal("audio playback is not initialized");
    }
    return playback_->PlaySamples(audio_for_playback->samples.data(),
                                  audio_for_playback->samples.size(),
                                  audio_for_playback->sample_rate);
  }

  Status PlayFile(const std::string& path) override {
    if (playback_ == nullptr) {
      return Status::Internal("audio playback is not initialized");
    }
    return playback_->PlayWavFile(path);
  }

  Status PlayTone(int frequency_hz, int duration_ms, float amplitude) override {
    if (playback_ == nullptr) {
      return Status::Internal("audio playback is not initialized");
    }

    const int sample_rate = 44100;
    const int freq = std::max(80, frequency_hz);
    const int duration = std::max(50, duration_ms);
    const float amp = std::max(0.01F, std::min(1.0F, amplitude));
    const std::size_t total_samples =
        static_cast<std::size_t>((static_cast<long long>(sample_rate) * duration) / 1000LL);
    if (total_samples == 0U) {
      return Status::Ok();
    }

    std::vector<float> tone(total_samples, 0.0F);
    const double pi = std::acos(-1.0);
    const double phase_step = (2.0 * pi * static_cast<double>(freq)) / static_cast<double>(sample_rate);
    const std::size_t fade_samples =
        std::min<std::size_t>(total_samples / 4U, static_cast<std::size_t>(sample_rate / 100U));  // 10ms max

    double phase = 0.0;
    for (std::size_t i = 0; i < total_samples; ++i) {
      float env = 1.0F;
      if (fade_samples > 0U) {
        if (i < fade_samples) {
          env = static_cast<float>(i) / static_cast<float>(fade_samples);
        } else if ((total_samples - i) < fade_samples) {
          env = static_cast<float>(total_samples - i) / static_cast<float>(fade_samples);
        }
      }
      tone[i] = amp * env * static_cast<float>(std::sin(phase));
      phase += phase_step;
    }

    return playback_->PlaySamples(tone.data(), tone.size(), sample_rate);
  }

  Status PreloadFixedPhrases(const std::vector<std::string>& texts) override {
    if (!config_.fixed_phrase_cache || texts.empty()) {
      return Status::Ok();
    }
    if (tts_ == nullptr) {
      return Status::Internal("TTS not initialized");
    }

    std::unordered_set<std::string> unique_texts;
    std::size_t loaded = 0;
    for (const auto& text : texts) {
      if (text.empty() || !unique_texts.insert(text).second) {
        continue;
      }
      if (fixed_phrase_cache_.find(text) != fixed_phrase_cache_.end()) {
        continue;
      }
      CachedAudio cached;
      Status gen_st = GenerateAudio(text, &cached);
      if (!gen_st.ok()) {
        LogWarn(logevent::kSystemBoot, LogContext{"TtsEngine", "", 0, "", ""},
                {Kv("detail", "fixed_phrase_preload_failed"),
                 Kv("text", MaskSummary(text, 16)),
                 Kv("err", gen_st.message())});
        continue;
      }
      fixed_phrase_cache_[text] = std::move(cached);
      ++loaded;
    }

    LogInfo(logevent::kSystemBoot, LogContext{"TtsEngine", "", 0, "", ""},
            {Kv("detail", "fixed_phrase_preload_done"),
             Kv("count", static_cast<unsigned long long>(loaded))});
    return Status::Ok();
  }

 private:
  struct CachedAudio {
    std::vector<float> samples;
    int sample_rate = 0;
  };

  Status GenerateAudio(const std::string& text, CachedAudio* out) {
    if (out == nullptr) {
      return Status::InvalidArgument("cached audio output is null");
    }
    SherpaOnnxGenerationConfig gen_cfg;
    std::memset(&gen_cfg, 0, sizeof(gen_cfg));
    gen_cfg.sid = 0;
    gen_cfg.speed = 1.0F;
    gen_cfg.silence_scale = 0.2F;

    const SherpaOnnxGeneratedAudio* audio =
        SherpaOnnxOfflineTtsGenerateWithConfig(
            tts_, text.c_str(), &gen_cfg, nullptr, nullptr);
    if (audio == nullptr) {
      return Status::Internal("SherpaOnnxOfflineTtsGenerateWithConfig failed");
    }

    out->sample_rate = static_cast<int>(audio->sample_rate);
    out->samples.assign(audio->samples,
                        audio->samples + static_cast<std::size_t>(audio->n));
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    return Status::Ok();
  }

  TtsConfig config_;
  const SherpaOnnxOfflineTts* tts_ = nullptr;
  std::unique_ptr<AudioPlayback> playback_;
  std::unordered_map<std::string, CachedAudio> fixed_phrase_cache_;
};

#else

class SherpaTtsEngine final : public TtsEngine {
 public:
  Status Initialize(const TtsConfig&, const AudioConfig&) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status Speak(const std::string&) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status PlayFile(const std::string&) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status PlayTone(int, int, float) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
};

#endif

}  // namespace

std::unique_ptr<TtsEngine> CreateTtsEngine() {
  return std::make_unique<SherpaTtsEngine>();
}

}  // namespace mos::vis

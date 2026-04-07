#include "mos/vis/tts/tts_engine.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

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

    GetLogger()->info(
        "TTS initialized: model={} use_int8={} int8_size={} data_dir={}",
        model,
        (model == model_int8) ? "true" : "false",
        static_cast<unsigned long long>(int8_size),
        has_vits_data_dir ? data_dir : "<none>");
    return Status::Ok();
  }

  Status Speak(const std::string& text) override {
    if (text.empty()) {
      return Status::Ok();
    }
    if (tts_ == nullptr) {
      return Status::Internal("TTS not initialized");
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

    Status st = Status::Internal("audio playback is not initialized");
    if (playback_ != nullptr) {
      st = playback_->PlaySamples(audio->samples,
                                  static_cast<std::size_t>(audio->n),
                                  static_cast<int>(audio->sample_rate));
    }
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    return st;
  }

  Status PlayFile(const std::string& path) override {
    if (playback_ == nullptr) {
      return Status::Internal("audio playback is not initialized");
    }
    return playback_->PlayWavFile(path);
  }

 private:
  TtsConfig config_;
  const SherpaOnnxOfflineTts* tts_ = nullptr;
  std::unique_ptr<AudioPlayback> playback_;
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
};

#endif

}  // namespace

std::unique_ptr<TtsEngine> CreateTtsEngine() {
  return std::make_unique<SherpaTtsEngine>();
}

}  // namespace mos::vis

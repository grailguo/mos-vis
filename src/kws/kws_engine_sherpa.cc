#include "mos/vis/kws/kws_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mos/vis/common/logging.h"

#ifdef MOS_VIS_HAS_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace mos::vis {
namespace {

#ifdef MOS_VIS_HAS_SHERPA_ONNX

std::string FindFirstExistingFile(const std::filesystem::path& dir,
                                  const std::vector<std::string>& candidates) {
  for (const auto& name : candidates) {
    const auto p = (dir / name).lexically_normal();
    if (std::filesystem::exists(p)) {
      return p.string();
    }
  }
  return "";
}

class SherpaKwsEngine final : public KwsEngine {
 public:
  SherpaKwsEngine() = default;
  ~SherpaKwsEngine() override {
    if (stream_ != nullptr) {
      SherpaOnnxDestroyOnlineStream(stream_);
      stream_ = nullptr;
    }
    if (spotter_ != nullptr) {
      SherpaOnnxDestroyKeywordSpotter(spotter_);
      spotter_ = nullptr;
    }
  }

  Status Initialize(const KwsConfig& config) override {
    config_ = config;
    const std::filesystem::path model_dir(config.model_dir);

    if (!std::filesystem::exists(model_dir)) {
      return Status::NotFound("KWS model_dir not found: " + config.model_dir);
    }

    encoder_path_ = FindFirstExistingFile(
        model_dir,
        {"encoder-epoch-13-avg-2-chunk-16-left-64.int8.onnx",
         "encoder-epoch-13-avg-2-chunk-8-left-64.int8.onnx",
         "encoder-epoch-13-avg-2-chunk-16-left-64.onnx",
         "encoder-epoch-13-avg-2-chunk-8-left-64.onnx"});
    decoder_path_ = FindFirstExistingFile(
        model_dir,
        {"decoder-epoch-13-avg-2-chunk-16-left-64.onnx",
         "decoder-epoch-13-avg-2-chunk-8-left-64.onnx"});
    joiner_path_ = FindFirstExistingFile(
        model_dir,
        {"joiner-epoch-13-avg-2-chunk-16-left-64.int8.onnx",
         "joiner-epoch-13-avg-2-chunk-8-left-64.int8.onnx",
         "joiner-epoch-13-avg-2-chunk-16-left-64.onnx",
         "joiner-epoch-13-avg-2-chunk-8-left-64.onnx"});
    tokens_path_ = FindFirstExistingFile(model_dir, {"tokens.txt"});
    keywords_path_ = FindFirstExistingFile(model_dir, {"keywords.txt", "test_wavs/keywords.txt"});

    if (encoder_path_.empty() || decoder_path_.empty() || joiner_path_.empty() ||
        tokens_path_.empty() || keywords_path_.empty()) {
      return Status::NotFound(
          "KWS model files missing. Need encoder/decoder/joiner/tokens/keywords in " +
          config.model_dir);
    }

    SherpaOnnxKeywordSpotterConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    cfg.model_config.transducer.encoder = encoder_path_.c_str();
    cfg.model_config.transducer.decoder = decoder_path_.c_str();
    cfg.model_config.transducer.joiner = joiner_path_.c_str();
    cfg.model_config.tokens = tokens_path_.c_str();
    cfg.model_config.num_threads = 1;
    cfg.model_config.provider = "cpu";
    cfg.model_config.debug = 0;
    cfg.max_active_paths = 4;
    cfg.num_trailing_blanks = 1;
    cfg.keywords_score = 1.5F;
    cfg.keywords_threshold = 0.25F;
    cfg.keywords_file = keywords_path_.c_str();

    spotter_ = SherpaOnnxCreateKeywordSpotter(&cfg);
    if (spotter_ == nullptr) {
      return Status::Internal("SherpaOnnxCreateKeywordSpotter failed");
    }

    stream_ = SherpaOnnxCreateKeywordStream(spotter_);
    if (stream_ == nullptr) {
      SherpaOnnxDestroyKeywordSpotter(spotter_);
      spotter_ = nullptr;
      return Status::Internal("SherpaOnnxCreateKeywordStream failed");
    }

    GetLogger()->info("KWS initialized: model_dir={} keywords_file={}",
                      config.model_dir,
                      keywords_path_);
    return Status::Ok();
  }

  Status Reset() override {
    if (spotter_ == nullptr || stream_ == nullptr) {
      return Status::Internal("KWS not initialized");
    }
    SherpaOnnxResetKeywordStream(spotter_, stream_);
    return Status::Ok();
  }

  Status Process(const float* samples, std::size_t count, KwsResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("kws result is null");
    }
    result->detected = false;
    result->keyword.clear();
    result->json.clear();
    if (samples == nullptr) {
      return Status::InvalidArgument("kws input samples is null");
    }
    if (spotter_ == nullptr || stream_ == nullptr) {
      return Status::Internal("KWS not initialized");
    }

    SherpaOnnxOnlineStreamAcceptWaveform(
        stream_, 16000, samples, static_cast<int32_t>(count));
    while (SherpaOnnxIsKeywordStreamReady(spotter_, stream_) != 0) {
      SherpaOnnxDecodeKeywordStream(spotter_, stream_);
    }

    const SherpaOnnxKeywordResult* r = SherpaOnnxGetKeywordResult(spotter_, stream_);
    if (r != nullptr && r->keyword != nullptr && std::strlen(r->keyword) > 0U) {
      result->detected = true;
      result->keyword = r->keyword;
      if (r->json != nullptr) {
        result->json = r->json;
      }
      SherpaOnnxResetKeywordStream(spotter_, stream_);
    }
    if (r != nullptr) {
      SherpaOnnxDestroyKeywordResult(r);
    }
    return Status::Ok();
  }

 private:
  KwsConfig config_;
  std::string encoder_path_;
  std::string decoder_path_;
  std::string joiner_path_;
  std::string tokens_path_;
  std::string keywords_path_;
  const SherpaOnnxKeywordSpotter* spotter_ = nullptr;
  const SherpaOnnxOnlineStream* stream_ = nullptr;
};

#else

class SherpaKwsEngine final : public KwsEngine {
 public:
  Status Initialize(const KwsConfig&) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status Reset() override { return Status::Ok(); }
  Status Process(const float*, std::size_t, KwsResult*) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
};

#endif

}  // namespace

std::unique_ptr<KwsEngine> CreateKwsEngine() {
  return std::make_unique<SherpaKwsEngine>();
}

}  // namespace mos::vis

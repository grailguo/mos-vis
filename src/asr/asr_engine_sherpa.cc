#include "mos/vis/asr/asr_engine.h"

#include <cstddef>
#include <cstdint>
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

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

class SherpaAsrEngine final : public AsrEngine {
 public:
  SherpaAsrEngine() = default;

  ~SherpaAsrEngine() override {
    if (stream_ != nullptr) {
      SherpaOnnxDestroyOnlineStream(stream_);
      stream_ = nullptr;
    }
    if (recognizer_ != nullptr) {
      SherpaOnnxDestroyOnlineRecognizer(recognizer_);
      recognizer_ = nullptr;
    }
  }

  Status Initialize(const AsrConfig& config) override {
    config_ = config;
    const std::filesystem::path model_dir(config.model_dir);
    if (!std::filesystem::exists(model_dir)) {
      return Status::NotFound("ASR model_dir not found: " + config.model_dir);
    }

    encoder_path_ = FindFirstExistingFile(
        model_dir,
        {"encoder.rknn", "encoder.int8.onnx", "encoder.onnx",
         "encoder-epoch-99-avg-1.int8.onnx", "encoder-epoch-99-avg-1.onnx"});
    decoder_path_ = FindFirstExistingFile(
        model_dir,
        {"decoder.rknn", "decoder.onnx", "decoder-epoch-99-avg-1.onnx"});
    joiner_path_ = FindFirstExistingFile(
        model_dir,
        {"joiner.rknn", "joiner.int8.onnx", "joiner.onnx",
         "joiner-epoch-99-avg-1.int8.onnx", "joiner-epoch-99-avg-1.onnx"});
    tokens_path_ = FindFirstExistingFile(model_dir, {"tokens.txt"});

    if (encoder_path_.empty() || decoder_path_.empty() || joiner_path_.empty() ||
        tokens_path_.empty()) {
      return Status::NotFound(
          "ASR model files missing. Need encoder/decoder/joiner/tokens in " +
          config.model_dir);
    }

    const bool use_rknn_model =
        EndsWith(encoder_path_, ".rknn") &&
        EndsWith(decoder_path_, ".rknn") &&
        EndsWith(joiner_path_, ".rknn");

    SherpaOnnxOnlineRecognizerConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    cfg.model_config.transducer.encoder = encoder_path_.c_str();
    cfg.model_config.transducer.decoder = decoder_path_.c_str();
    cfg.model_config.transducer.joiner = joiner_path_.c_str();
    cfg.model_config.tokens = tokens_path_.c_str();
    cfg.model_config.num_threads = 1;
    cfg.model_config.provider = use_rknn_model ? "rknn" : "cpu";
    cfg.model_config.debug = 0;
    cfg.decoding_method = "greedy_search";
    cfg.max_active_paths = 4;
    cfg.enable_endpoint = 0;
    cfg.blank_penalty = 0.0F;

    recognizer_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
    if (recognizer_ == nullptr) {
      return Status::Internal("SherpaOnnxCreateOnlineRecognizer failed");
    }

    stream_ = SherpaOnnxCreateOnlineStream(recognizer_);
    if (stream_ == nullptr) {
      SherpaOnnxDestroyOnlineRecognizer(recognizer_);
      recognizer_ = nullptr;
      return Status::Internal("SherpaOnnxCreateOnlineStream failed");
    }

    last_text_.clear();
    last_json_.clear();
    GetLogger()->info("ASR initialized: provider={} encoder={} decoder={} joiner={}",
                      cfg.model_config.provider,
                      encoder_path_,
                      decoder_path_,
                      joiner_path_);
    return Status::Ok();
  }

  Status Reset() override {
    if (recognizer_ == nullptr || stream_ == nullptr) {
      return Status::Internal("ASR not initialized");
    }
    SherpaOnnxOnlineStreamReset(recognizer_, stream_);
    last_text_.clear();
    last_json_.clear();
    return Status::Ok();
  }

  Status AcceptAudio(const float* samples, std::size_t count) override {
    if (samples == nullptr) {
      return Status::InvalidArgument("ASR input samples is null");
    }
    if (recognizer_ == nullptr || stream_ == nullptr) {
      return Status::Internal("ASR not initialized");
    }
    SherpaOnnxOnlineStreamAcceptWaveform(
        stream_, 16000, samples, static_cast<int32_t>(count));
    return Status::Ok();
  }

  Status DecodeAvailable() override {
    if (recognizer_ == nullptr || stream_ == nullptr) {
      return Status::Internal("ASR not initialized");
    }
    while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_) != 0) {
      SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
    }
    ReadCurrentResult();
    return Status::Ok();
  }

  Status GetResult(AsrResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("ASR result is null");
    }
    result->text = last_text_;
    result->json = last_json_;
    result->is_final = false;
    return Status::Ok();
  }

  Status FinalizeAndFlush(AsrResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("ASR result is null");
    }
    if (recognizer_ == nullptr || stream_ == nullptr) {
      return Status::Internal("ASR not initialized");
    }

    SherpaOnnxOnlineStreamInputFinished(stream_);
    while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_) != 0) {
      SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
    }
    ReadCurrentResult();

    result->text = last_text_;
    result->json = last_json_;
    result->is_final = true;

    SherpaOnnxOnlineStreamReset(recognizer_, stream_);
    return Status::Ok();
  }

 private:
  void ReadCurrentResult() {
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
    if (r != nullptr) {
      last_text_ = (r->text != nullptr) ? r->text : "";
      last_json_ = (r->json != nullptr) ? r->json : "";
      SherpaOnnxDestroyOnlineRecognizerResult(r);
    }
  }

  AsrConfig config_;
  std::string encoder_path_;
  std::string decoder_path_;
  std::string joiner_path_;
  std::string tokens_path_;
  std::string last_text_;
  std::string last_json_;
  const SherpaOnnxOnlineRecognizer* recognizer_ = nullptr;
  const SherpaOnnxOnlineStream* stream_ = nullptr;
};

#else

class SherpaAsrEngine final : public AsrEngine {
 public:
  Status Initialize(const AsrConfig&) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status Reset() override { return Status::Ok(); }
  Status AcceptAudio(const float*, std::size_t) override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status DecodeAvailable() override {
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status GetResult(AsrResult* result) override {
    if (result != nullptr) {
      result->text.clear();
      result->json.clear();
      result->is_final = false;
    }
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
  Status FinalizeAndFlush(AsrResult* result) override {
    if (result != nullptr) {
      result->text.clear();
      result->json.clear();
      result->is_final = true;
    }
    return Status::Internal("Sherpa-ONNX runtime not enabled");
  }
};

#endif

}  // namespace

std::unique_ptr<AsrEngine> CreateAsrEngine() {
  return std::make_unique<SherpaAsrEngine>();
}

}  // namespace mos::vis

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {

class TtsEngine {
 public:
  virtual ~TtsEngine() = default;

  virtual Status Initialize(const TtsConfig& config, const AudioConfig& audio_config) = 0;
  virtual Status Speak(const std::string& text) = 0;
  virtual Status PlayFile(const std::string& path) = 0;
  virtual Status PlayTone(int frequency_hz, int duration_ms, float amplitude) {
    (void)frequency_hz;
    (void)duration_ms;
    (void)amplitude;
    return Status::Internal("TTS tone playback is not supported");
  }
  virtual Status PreloadFixedPhrases(const std::vector<std::string>& texts) {
    (void)texts;
    return Status::Ok();
  }
};

std::unique_ptr<TtsEngine> CreateTtsEngine();
std::unique_ptr<TtsEngine> CreateVitsMeloTtsEngine();

}  // namespace mos::vis

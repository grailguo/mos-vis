#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {

class AudioPlayback {
 public:
  virtual ~AudioPlayback() = default;

  virtual Status Initialize(const AudioConfig& config) = 0;
  virtual Status Start() = 0;
  virtual void Stop() = 0;

  virtual Status PlaySamples(const float* samples,
                             std::size_t count,
                             int sample_rate_hz) = 0;
  virtual Status PlayWavFile(const std::string& path) = 0;
};

std::unique_ptr<AudioPlayback> CreateAudioPlayback();

}  // namespace mos::vis

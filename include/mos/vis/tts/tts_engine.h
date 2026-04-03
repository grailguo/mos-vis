#pragma once

#include <memory>
#include <string>

#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {

class TtsEngine {
 public:
  virtual ~TtsEngine() = default;

  virtual Status Initialize(const TtsConfig& config) = 0;
  virtual Status Speak(const std::string& text) = 0;
  virtual Status PlayFile(const std::string& path) = 0;
};

std::unique_ptr<TtsEngine> CreateTtsEngine();

}  // namespace mos::vis

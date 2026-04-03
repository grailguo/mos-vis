#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {

struct AsrResult {
  std::string text;
  std::string json;
  bool is_final = false;
};

class AsrEngine {
 public:
  virtual ~AsrEngine() = default;

  virtual Status Initialize(const AsrConfig& config) = 0;
  virtual Status Reset() = 0;
  virtual Status AcceptAudio(const float* samples, std::size_t count) = 0;
  virtual Status DecodeAvailable() = 0;
  virtual Status GetResult(AsrResult* result) = 0;
  virtual Status FinalizeAndFlush(AsrResult* result) = 0;
};

std::unique_ptr<AsrEngine> CreateAsrEngine();

}  // namespace mos::vis

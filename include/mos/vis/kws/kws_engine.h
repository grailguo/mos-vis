#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {

struct KwsResult {
  bool detected = false;
  std::string keyword;
  std::string json;
};

class KwsEngine {
 public:
  virtual ~KwsEngine() = default;

  virtual Status Initialize(const KwsConfig& config) = 0;
  virtual Status Reset() = 0;
  virtual Status Process(const float* samples, std::size_t count, KwsResult* result) = 0;
};

std::unique_ptr<KwsEngine> CreateKwsEngine();

}  // namespace mos::vis

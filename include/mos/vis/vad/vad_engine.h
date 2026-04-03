
#pragma once

#include <cstddef>
#include <memory>

#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {

struct VadResult {
  bool speech = false;
  float probability = 0.0F;
};

class VadEngine {
 public:
  virtual ~VadEngine() = default;

  virtual Status Initialize(const VadConfig& config) = 0;
  virtual Status Reset() = 0;
  virtual Status Process(const float* samples, std::size_t count, VadResult* result) = 0;
};

std::unique_ptr<VadEngine> CreateVadEngine();

}  // namespace mos::vis

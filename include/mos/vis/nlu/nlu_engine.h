#pragma once

#include <memory>
#include <string>

#include "mos/vis/common/status.h"

namespace mos::vis {

struct NluConfig {
  bool enabled = true;
  std::string model_dir;
  std::string provider = "cpu";
  int num_threads = 1;
};

struct NluResult {
  std::string intent;
  float confidence = 0.0F;
  std::string reply_text;
  std::string json;
};

class NluEngine {
 public:
  virtual ~NluEngine() = default;

  virtual Status Initialize(const NluConfig& config) = 0;
  virtual Status Reset() = 0;
  virtual Status Infer(const std::string& text, NluResult* result) = 0;
};

std::unique_ptr<NluEngine> CreateNluEngine();

}  // namespace mos::vis

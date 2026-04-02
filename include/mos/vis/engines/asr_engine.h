#pragma once

#include <string>

namespace mos::vis {

class AsrEngine {
 public:
  virtual ~AsrEngine() = default;
  virtual std::string Transcribe(const float* audio, int size) = 0;
};

class StubAsrEngine final : public AsrEngine {
 public:
  std::string Transcribe(const float* audio, int size) override;
};

}  // namespace mos::vis

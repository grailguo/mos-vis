#pragma once

namespace mos::vis {

class VadEngine {
 public:
  virtual ~VadEngine() = default;
  virtual bool IsSpeechFrame(const float* frame, int size) = 0;
};

class StubVadEngine final : public VadEngine {
 public:
  bool IsSpeechFrame(const float* frame, int size) override;
};

}  // namespace mos::vis

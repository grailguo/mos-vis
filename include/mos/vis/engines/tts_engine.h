#pragma once

#include <string>

namespace mos::vis {

class TtsEngine {
 public:
  virtual ~TtsEngine() = default;
  virtual void Speak(const std::string& text) = 0;
};

class StubTtsEngine final : public TtsEngine {
 public:
  void Speak(const std::string& text) override;
};

}  // namespace mos::vis

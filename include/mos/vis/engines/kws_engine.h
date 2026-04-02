#pragma once

#include <string>

namespace mos::vis {

class KwsEngine {
 public:
  virtual ~KwsEngine() = default;
  virtual bool Detect(const std::string& text) = 0;
};

class StubKwsEngine final : public KwsEngine {
 public:
  bool Detect(const std::string& text) override;
};

}  // namespace mos::vis

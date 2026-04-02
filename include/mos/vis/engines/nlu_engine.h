#pragma once

#include <string>

#include "mos/vis/engines/intent.h"

namespace mos::vis {

class NluEngine {
 public:
  virtual ~NluEngine() = default;
  virtual IntentResult Parse(const std::string& text) = 0;
};

class StubNluEngine final : public NluEngine {
 public:
  IntentResult Parse(const std::string& text) override;
};

}  // namespace mos::vis

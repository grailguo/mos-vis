#pragma once

#include <string>

#include "mos/vis/engines/intent.h"

namespace mos::vis {

class PipelineRouter {
 public:
  std::string Route(const IntentResult& intent) const;
};

}  // namespace mos::vis

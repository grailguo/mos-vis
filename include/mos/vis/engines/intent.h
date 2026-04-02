#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mos::vis {

enum class IntentType {
  kUnknown,
  kWakeup,
  kDeviceControl,
  kKnowledgeQuery,
  kChat,
};

struct IntentResult {
  IntentType intent = IntentType::kUnknown;
  std::string text;
  std::string action;
  nlohmann::json slots;
};

}  // namespace mos::vis

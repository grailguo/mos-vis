#pragma once

#include <memory>
#include <string>

#include "mos/vis/common/status.h"

namespace mos::vis {
struct ControlConfig;

struct ControlRequest {
  std::string intent;
  float confidence = 0.0F;
  std::string text;
  std::string nlu_json;
};

struct ControlResult {
  bool handled = false;
  std::string action;
  std::string ws_payload_json;
  std::string reply_text;
};

class ControlEngine {
 public:
  virtual ~ControlEngine() = default;

  virtual Status Initialize() = 0;
  virtual Status Reset() = 0;
  virtual Status Execute(const ControlRequest& request, ControlResult* result) = 0;
  virtual Status PollNotification(ControlResult* result) = 0;
};

std::unique_ptr<ControlEngine> CreateControlEngine(const ControlConfig& config);

}  // namespace mos::vis

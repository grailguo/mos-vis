#pragma once

#include <memory>
#include <string>

#include "mos/vis/control/control_engine.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief Control notification pipeline stage.
 *
 * This stage polls the ControlEngine for asynchronous notifications (e.g.,
 * device status updates) and schedules TTS tasks for any received messages.
 */
class ControlNotificationStage : public PipelineStage {
 public:
  ControlNotificationStage();
  ~ControlNotificationStage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "ControlNotificationStage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;
};

}  // namespace mos::vis
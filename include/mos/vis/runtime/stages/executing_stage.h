#pragma once

#include <memory>
#include <string>

#include "mos/vis/control/control_engine.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief Executing pipeline stage for control command execution.
 *
 * This stage takes the NLU result and executes control commands via the
 * ControlEngine. It also handles unknown intents and schedules TTS responses.
 */
class ExecutingStage : public PipelineStage {
 public:
  ExecutingStage();
  ~ExecutingStage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "ExecutingStage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Execute control command via ControlEngine
  Status ExecuteControl(SessionContext& context,
                        const ControlRequest& request,
                        std::string& reply_text);
};

}  // namespace mos::vis
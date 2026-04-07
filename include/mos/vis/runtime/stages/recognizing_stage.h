#pragma once

#include <memory>
#include <string>

#include "mos/vis/nlu/nlu_engine.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief Recognizing pipeline stage for natural language understanding.
 *
 * This stage takes the final ASR text and runs NLU inference to determine
 * user intent. It distinguishes between control commands and knowledge queries,
 * and prepares the appropriate response or action.
 */
class RecognizingStage : public PipelineStage {
 public:
  RecognizingStage();
  ~RecognizingStage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "RecognizingStage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Helper to trim ASCII whitespace
  static std::string TrimAsciiWhitespace(const std::string& s);
};

}  // namespace mos::vis
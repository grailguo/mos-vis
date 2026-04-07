#pragma once

#include <string>

#include "mos/vis/common/status.h"
#include "mos/vis/runtime/session_state.h"

namespace mos::vis {

// Forward declaration
struct SessionContext;

/**
 * @brief Base class for all pipeline stages in the voice interaction system.
 *
 * PipelineStage represents a single processing step in the voice interaction
 * pipeline (e.g., VAD1, KWS, ASR, NLU, TTS). Each stage implements the Process()
 * method to perform its specific functionality and CanProcess() to indicate
 * when it should be active based on the current session state.
 */
class PipelineStage {
 public:
  virtual ~PipelineStage() = default;

  /**
   * @brief Check if this stage can process in the given session state.
   *
   * The pipeline scheduler calls this method to determine which stages
   * should be executed during the current tick.
   *
   * @param state Current session state
   * @return true if this stage should be executed, false otherwise
   */
  virtual bool CanProcess(SessionState state) const = 0;

  /**
   * @brief Process one tick of this pipeline stage.
   *
   * Performs the stage's specific processing logic. The stage may read
   * from and write to the shared SessionContext.
   *
   * @param context Shared context containing session state, engines, and data
   * @return Status indicating success or failure
   */
  virtual Status Process(SessionContext& context) = 0;

  /**
   * @brief Get the name of this pipeline stage for logging and debugging.
   *
   * @return std::string Stage name
   */
  virtual std::string Name() const = 0;

  /**
   * @brief Optional method called when the stage is added to the pipeline.
   *
   * Can be used for initialization that requires access to SessionContext.
   *
   * @param context Shared context
   * @return Status indicating success or failure
   */
  virtual Status OnAttach(SessionContext& context) { return Status::Ok(); }

  /**
   * @brief Optional method called when the stage is removed from the pipeline.
   *
   * Can be used for cleanup.
   *
   * @param context Shared context
   */
  virtual void OnDetach(SessionContext& context) {}

 protected:
  PipelineStage() = default;
};

}  // namespace mos::vis
#pragma once

#include <memory>
#include <vector>

#include "mos/vis/common/status.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief Manages and executes pipeline stages in the voice interaction system.
 *
 * PipelineScheduler coordinates the execution of pipeline stages based on the
 * current session state. It owns the pipeline stage instances and ensures
 * they are executed in the correct order with proper error handling.
 */
class PipelineScheduler {
 public:
  PipelineScheduler();
  ~PipelineScheduler();

  // Prevent copying
  PipelineScheduler(const PipelineScheduler&) = delete;
  PipelineScheduler& operator=(const PipelineScheduler&) = delete;

  /**
   * @brief Initialize the scheduler with engine instances and configuration.
   *
   * @param context Session context with engines and configuration
   * @return Status indicating success or failure
   */
  Status Initialize(SessionContext& context);

  /**
   * @brief Execute one tick of the pipeline.
   *
   * Processes all active pipeline stages in order. A stage is active if
   * its CanProcess() method returns true for the current session state.
   *
   * @param context Shared session context
   * @return Status indicating success or failure of the pipeline tick
   */
  Status Process(SessionContext& context);

  /**
   * @brief Add a pipeline stage to the scheduler.
   *
   * The stage will be executed in the order it was added. Ownership
   * of the stage is transferred to the scheduler.
   *
   * @param stage Unique pointer to the pipeline stage
   */
  void AddStage(std::unique_ptr<PipelineStage> stage);

  /**
   * @brief Get the number of pipeline stages managed by the scheduler.
   *
   * @return size_t Number of stages
   */
  size_t GetStageCount() const;

  /**
   * @brief Get a stage by index (for testing and debugging).
   *
   * @param index Stage index (0-based)
   * @return Pointer to the stage, or nullptr if index is out of bounds
   */
  const PipelineStage* GetStage(size_t index) const;

  /**
   * @brief Clear all pipeline stages.
   *
   * Useful for reconfiguration or shutdown.
   *
   * @param context Shared session context for stage cleanup
   */
  void ClearStages(SessionContext& context);

  /**
   * @brief Reset all pipeline stages to their initial state.
   *
   * Calls Reset() on each stage if available, or OnDetach/OnAttach.
   *
   * @param context Shared session context
   */
  void Reset(SessionContext& context);

 private:
  std::vector<std::unique_ptr<PipelineStage>> stages_;
  bool initialized_ = false;
};

}  // namespace mos::vis
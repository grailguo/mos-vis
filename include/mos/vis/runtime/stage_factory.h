#pragma once

#include <memory>
#include <vector>

#include "mos/vis/runtime/pipeline_scheduler.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief Factory for creating and configuring pipeline stages.
 *
 * StageFactory creates the complete set of pipeline stages in the correct
 * order and adds them to a PipelineScheduler. It encapsulates the knowledge
 * of which stages are needed and their dependencies.
 */
class StageFactory {
 public:
  StageFactory() = default;
  ~StageFactory() = default;

  // Prevent copying
  StageFactory(const StageFactory&) = delete;
  StageFactory& operator=(const StageFactory&) = delete;

  /**
   * @brief Create all pipeline stages and add them to the scheduler.
   *
   * The stages are added in the order they should be executed each tick.
   *
   * @param scheduler PipelineScheduler to populate with stages
   * @param context SessionContext (used for stage initialization)
   * @return Status indicating success or failure
   */
  static Status CreateStages(PipelineScheduler* scheduler, SessionContext& context);
};

}  // namespace mos::vis
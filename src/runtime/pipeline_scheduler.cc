#include "mos/vis/runtime/pipeline_scheduler.h"

#include <algorithm>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "PipelineScheduler";

}  // namespace

PipelineScheduler::PipelineScheduler() = default;

PipelineScheduler::~PipelineScheduler() = default;

Status PipelineScheduler::Initialize(SessionContext& context) {
  if (initialized_) {
    GetLogger()->warn("{}: Already initialized", kLogTag);
    return Status::Ok();
  }

  GetLogger()->info("{}: Initializing with {} stages", kLogTag, stages_.size());

  // Call OnAttach for each stage
  for (auto& stage : stages_) {
    Status st = stage->OnAttach(context);
    if (!st.ok()) {
      GetLogger()->error("{}: Stage '{}' failed to attach: {}", kLogTag,
                         stage->Name(), st.message());
      return Status::Internal(
          "Pipeline stage '" + stage->Name() + "' failed to attach: " + st.message());
    }
  }

  initialized_ = true;
  GetLogger()->info("{}: Initialization complete", kLogTag);
  return Status::Ok();
}

Status PipelineScheduler::Process(SessionContext& context) {
  if (!initialized_) {
    return Status::Internal("PipelineScheduler not initialized");
  }

  // Process stages in the order they were added
  for (auto& stage : stages_) {
    if (stage->CanProcess(context.state)) {
      Status st = stage->Process(context);
      if (!st.ok()) {
        GetLogger()->error("{}: Stage '{}' failed: {}", kLogTag,
                           stage->Name(), st.message());
        // Continue processing other stages even if one fails
        // The stage should handle its own error recovery
      }
    }
  }

  return Status::Ok();
}

void PipelineScheduler::AddStage(std::unique_ptr<PipelineStage> stage) {
  if (!stage) {
    GetLogger()->warn("{}: Attempted to add null stage", kLogTag);
    return;
  }

  if (initialized_) {
    GetLogger()->warn("{}: Adding stage after initialization; stage may not be attached",
                      kLogTag);
  }

  GetLogger()->debug("{}: Adding pipeline stage: {}", kLogTag, stage->Name());
  stages_.push_back(std::move(stage));
}

size_t PipelineScheduler::GetStageCount() const {
  return stages_.size();
}

const PipelineStage* PipelineScheduler::GetStage(size_t index) const {
  if (index >= stages_.size()) {
    return nullptr;
  }
  return stages_[index].get();
}

void PipelineScheduler::ClearStages(SessionContext& context) {
  if (initialized_) {
    // Call OnDetach for each stage before clearing
    for (auto& stage : stages_) {
      stage->OnDetach(context);
    }
  }
  stages_.clear();
  initialized_ = false;
  GetLogger()->info("{}: Cleared all pipeline stages", kLogTag);
}

void PipelineScheduler::Reset(SessionContext& context) {
  GetLogger()->debug("{}: Resetting all pipeline stages", kLogTag);

  // Call OnDetach and then OnAttach for each stage
  for (auto& stage : stages_) {
    stage->OnDetach(context);
    Status st = stage->OnAttach(context);
    if (!st.ok()) {
      GetLogger()->error("{}: Stage '{}' failed to re-attach during reset: {}",
                         kLogTag, stage->Name(), st.message());
    }
  }

  GetLogger()->info("{}: Reset complete", kLogTag);
}

}  // namespace mos::vis
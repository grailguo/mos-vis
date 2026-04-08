#include "mos/vis/runtime/pipeline_scheduler.h"

#include <algorithm>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "PipelineScheduler";

LogContext MakeLogCtx(const SessionContext& context) {
  LogContext ctx;
  ctx.module = kLogTag;
  ctx.session = context.session_id;
  ctx.turn = context.turn_id;
  ctx.state = std::to_string(static_cast<int>(context.state));
  ctx.req = context.current_control_request_id;
  return ctx;
}

}  // namespace

PipelineScheduler::PipelineScheduler() = default;

PipelineScheduler::~PipelineScheduler() = default;

Status PipelineScheduler::Initialize(SessionContext& context) {
  if (initialized_) {
    LogWarn(logevent::kSystemBoot, MakeLogCtx(context), {Kv("detail", "already_initialized")});
    return Status::Ok();
  }

  LogInfo(logevent::kSystemBoot, MakeLogCtx(context),
          {Kv("detail", "pipeline_init"), Kv("stage_count", stages_.size())});

  // Call OnAttach for each stage
  for (auto& stage : stages_) {
    Status st = stage->OnAttach(context);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context),
               {Kv("event_detail", "stage_attach_failed"),
                Kv("stage", stage->Name()),
                Kv("err", st.message())});
      return Status::Internal(
          "Pipeline stage '" + stage->Name() + "' failed to attach: " + st.message());
    }
  }

  initialized_ = true;
  LogInfo(logevent::kSystemBoot, MakeLogCtx(context), {Kv("detail", "pipeline_init_done")});
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
        LogError(logevent::kSessionEnd, MakeLogCtx(context),
                 {Kv("event_detail", "stage_process_failed"),
                  Kv("stage", stage->Name()),
                  Kv("err", st.message())});
        // Continue processing other stages even if one fails
        // The stage should handle its own error recovery
      }
    }
  }

  return Status::Ok();
}

void PipelineScheduler::AddStage(std::unique_ptr<PipelineStage> stage) {
  if (!stage) {
    LogWarn(logevent::kSystemBoot, LogContext{std::string(kLogTag), "", 0, "", ""},
            {Kv("detail", "add_null_stage")});
    return;
  }

  if (initialized_) {
    LogWarn(logevent::kSystemBoot, LogContext{std::string(kLogTag), "", 0, "", ""},
            {Kv("detail", "add_stage_after_init"), Kv("stage", stage->Name())});
  }

  LogDebug(logevent::kSystemBoot, LogContext{std::string(kLogTag), "", 0, "", ""},
           {Kv("detail", "add_stage"), Kv("stage", stage->Name())});
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
  LogInfo(logevent::kSystemBoot, MakeLogCtx(context), {Kv("detail", "pipeline_stages_cleared")});
}

void PipelineScheduler::Reset(SessionContext& context) {
  LogDebug(logevent::kSystemBoot, MakeLogCtx(context), {Kv("detail", "pipeline_reset_begin")});

  // Call OnDetach and then OnAttach for each stage
  for (auto& stage : stages_) {
    stage->OnDetach(context);
    Status st = stage->OnAttach(context);
    if (!st.ok()) {
      LogError(logevent::kSessionEnd, MakeLogCtx(context),
               {Kv("event_detail", "stage_reattach_failed"),
                Kv("stage", stage->Name()),
                Kv("err", st.message())});
    }
  }

  LogInfo(logevent::kSystemBoot, MakeLogCtx(context), {Kv("detail", "pipeline_reset_done")});
}

}  // namespace mos::vis

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/kws/kws_engine.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief KWS pipeline stage for keyword spotting.
 *
 * This stage processes audio through the keyword spotting engine to detect
 * wake words. It integrates with the VAD1 gate to only process audio when
 * the gate is open (except for bypass during idle state). It also handles
 * preroll buffering and pending hit management.
 */
class KwsStage : public PipelineStage {
 public:
  KwsStage();
  ~KwsStage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "KwsStage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Handle a detected keyword hit
  void HandleKwsHit(SessionContext& context, const std::string& keyword,
                    const std::string& detail_json);
  void ConsumeWakeEvents(SessionContext& context,
                         const std::string& keyword,
                         const std::string& detail_json);

  // Audio reader for this stage
  std::unique_ptr<AudioReader> reader_;

  // Cached configuration values for performance
  std::size_t kws_chunk_samples_ = 0;
  std::size_t capture_chunk_samples_ = 0;
  std::size_t max_kws_chunks_per_tick_ = 0;

  // Temporary buffer for KWS processing
  std::vector<float> kws_chunk_buffer_;
};

}  // namespace mos::vis

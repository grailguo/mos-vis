#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/asr/asr_engine.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"

namespace mos::vis {

/**
 * @brief ASR pipeline stage for automatic speech recognition.
 *
 * This stage processes audio through the ASR engine when the session is in
 * listening or finalizing state. It handles partial results, timeouts, and
 * finalization when speech ends.
 */
class AsrStage : public PipelineStage {
 public:
  AsrStage();
  ~AsrStage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "AsrStage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Handle timeout when no new ASR text is received
  void HandleNoTextTimeout(SessionContext& context);

  // Process audio chunks while in listening/finalizing state
  Status ProcessAudioChunks(SessionContext& context);

  // Process tail audio after speech end (finalizing)
  Status ProcessTailAudio(SessionContext& context, std::size_t tail_samples);

  // Finalize ASR and transition to recognizing state
  Status FinalizeAsr(SessionContext& context);

  // Audio reader for this stage
  std::unique_ptr<AudioReader> reader_;

  // Cached configuration values for performance
  std::size_t asr_chunk_samples_ = 0;
  std::size_t max_chunks_per_tick_ = 8;

  // Temporary buffer for ASR processing
  std::vector<float> asr_chunk_buffer_;
};

}  // namespace mos::vis
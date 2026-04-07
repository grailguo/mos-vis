#pragma once

#include <memory>
#include <string>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"
#include "mos/vis/vad/vad_engine.h"

namespace mos::vis {

/**
 * @brief VAD1 pipeline stage for voice activity detection and KWS gate control.
 *
 * This stage processes audio through the first VAD engine to detect speech
 * presence and control the KWS gate. It implements the VAD1 state machine
 * that determines when keyword spotting should be active.
 */
class Vad1Stage : public PipelineStage {
 public:
  Vad1Stage();
  ~Vad1Stage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "Vad1Stage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Update the VAD1 state machine based on VAD probability
  bool UpdateStateMachine(SessionContext& context, float vad_probability);

  // Audio reader for this stage
  std::unique_ptr<AudioReader> reader_;

  // Cached configuration values for performance
  std::size_t vad_window_samples_ = 0;
  std::size_t vad_hop_samples_ = 0;
  std::size_t capture_chunk_samples_ = 0;
  std::size_t max_vad_windows_per_tick_ = 0;

  // Temporary buffer for VAD processing
  std::vector<float> vad_window_buffer_;
};

}  // namespace mos::vis
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"
#include "mos/vis/vad/vad_engine.h"

namespace mos::vis {

/**
 * @brief VAD2 pipeline stage for speech boundary detection.
 *
 * This stage processes audio through the second VAD engine to detect
 * speech start and end boundaries for ASR. It implements the VAD2 state
 * machine that determines when ASR should be active.
 */
class Vad2Stage : public PipelineStage {
 public:
  Vad2Stage();
  ~Vad2Stage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "Vad2Stage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Update the VAD2 state machine based on VAD probability
  bool UpdateStateMachine(SessionContext& context, float vad_probability);

  // Handle speech start event
  void OnSpeechStart(SessionContext& context);

  // Handle speech end event
  void OnSpeechEnd(SessionContext& context);

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

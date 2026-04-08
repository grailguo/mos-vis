#pragma once

#include <memory>
#include <string>

#include "mos/vis/runtime/pipeline_stage.h"
#include "mos/vis/runtime/session_context.h"
#include "mos/vis/tts/tts_engine.h"

namespace mos::vis {

/**
 * @brief TTS pipeline stage for text‑to‑speech synthesis.
 *
 * This stage processes pending TTS tasks, playing preset audio files or
 * synthesizing spoken replies. It manages the speaking state and transitions
 * back to pre‑listening or idle after playback completes.
 */
class TtsStage : public PipelineStage {
 public:
  TtsStage();
  ~TtsStage() override;

  // PipelineStage implementation
  bool CanProcess(SessionState state) const override;
  Status Process(SessionContext& context) override;
  std::string Name() const override { return "TtsStage"; }

  // Optional lifecycle methods
  Status OnAttach(SessionContext& context) override;
  void OnDetach(SessionContext& context) override;

 private:
  // Start a TTS task (preset file or synthesis)
  Status StartTask(SessionContext& context, const TtsTask& task);
  void ConsumeReplyEvents(SessionContext& context);
};

}  // namespace mos::vis

#pragma once

#include <string>

#include "mos/vis/engines/intent.h"

namespace mos::vis {

class WakeupPipeline {
 public:
  virtual ~WakeupPipeline() = default;
  virtual void Run() = 0;
};

class RecognitionPipeline {
 public:
  virtual ~RecognitionPipeline() = default;
  virtual std::string Run() = 0;
};

class ControlPipeline {
 public:
  virtual ~ControlPipeline() = default;
  virtual void Run(const IntentResult& intent) = 0;
};

class SpeakPipeline {
 public:
  virtual ~SpeakPipeline() = default;
  virtual void Run(const std::string& text) = 0;
};

class StubWakeupPipeline final : public WakeupPipeline {
 public:
  void Run() override;
};

class StubRecognitionPipeline final : public RecognitionPipeline {
 public:
  std::string Run() override;
};

class StubControlPipeline final : public ControlPipeline {
 public:
  void Run(const IntentResult& intent) override;
};

class StubSpeakPipeline final : public SpeakPipeline {
 public:
  void Run(const std::string& text) override;
};

}  // namespace mos::vis

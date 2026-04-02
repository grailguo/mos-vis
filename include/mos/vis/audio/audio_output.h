#pragma once

#include <cstddef>

namespace mos::vis {

class AudioOutput {
 public:
  virtual ~AudioOutput() = default;
  virtual bool Start() = 0;
  virtual void Stop() = 0;
  virtual std::size_t Write(const float* buffer, std::size_t frames) = 0;
};

class NullAudioOutput final : public AudioOutput {
 public:
  bool Start() override;
  void Stop() override;
  std::size_t Write(const float* buffer, std::size_t frames) override;

 private:
  bool running_ = false;
};

}  // namespace mos::vis

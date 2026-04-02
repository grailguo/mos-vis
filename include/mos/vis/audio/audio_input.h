#pragma once

#include <cstddef>

namespace mos::vis {

class AudioInput {
 public:
  virtual ~AudioInput() = default;
  virtual bool Start() = 0;
  virtual void Stop() = 0;
  virtual std::size_t Read(float* buffer, std::size_t frames) = 0;
};

class NullAudioInput final : public AudioInput {
 public:
  bool Start() override;
  void Stop() override;
  std::size_t Read(float* buffer, std::size_t frames) override;

 private:
  bool running_ = false;
};

}  // namespace mos::vis

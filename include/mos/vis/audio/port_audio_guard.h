#pragma once

namespace mos::vis {

class PortAudioGuard {
 public:
  PortAudioGuard();
  ~PortAudioGuard();

  PortAudioGuard(const PortAudioGuard&) = delete;
  PortAudioGuard& operator=(const PortAudioGuard&) = delete;

  bool IsInitialized() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace mos::vis

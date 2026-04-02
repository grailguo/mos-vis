#include "mos/vis/audio/port_audio_guard.h"

#include "mos/vis/audio/audio_input.h"
#include "mos/vis/audio/audio_output.h"

#include <spdlog/spdlog.h>

#if MOS_VIS_HAS_PORTAUDIO
#include <portaudio.h>
#endif

namespace mos::vis {

PortAudioGuard::PortAudioGuard() {
#if MOS_VIS_HAS_PORTAUDIO
  const PaError rc = Pa_Initialize();
  initialized_ = (rc == paNoError);
  if (!initialized_) {
    spdlog::warn("PortAudio initialization failed: {}", Pa_GetErrorText(rc));
  }
#else
  spdlog::info("PortAudio not available, using stub audio path");
  initialized_ = false;
#endif
}

PortAudioGuard::~PortAudioGuard() {
#if MOS_VIS_HAS_PORTAUDIO
  if (initialized_) {
    Pa_Terminate();
  }
#endif
}

bool NullAudioInput::Start() {
  running_ = true;
  return true;
}

void NullAudioInput::Stop() { running_ = false; }

std::size_t NullAudioInput::Read(float* buffer, std::size_t frames) {
  if (!running_ || buffer == nullptr) {
    return 0;
  }
  for (std::size_t i = 0; i < frames; ++i) {
    buffer[i] = 0.0f;
  }
  return frames;
}

bool NullAudioOutput::Start() {
  running_ = true;
  return true;
}

void NullAudioOutput::Stop() { running_ = false; }

std::size_t NullAudioOutput::Write(const float* buffer, std::size_t frames) {
  if (!running_ || buffer == nullptr) {
    return 0;
  }
  return frames;
}

}  // namespace mos::vis

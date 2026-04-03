#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "mos/vis/audio/audio_ring_buffer.h"
#include "mos/vis/common/status.h"
#include "mos/vis/config/app_config.h"

typedef void PaStream;
struct PaStreamCallbackTimeInfo;
using PaStreamCallbackFlags = unsigned long;

namespace mos::vis {

class AudioCapture {
 public:
  enum class ChannelSelectMode {
    kAutoOnce = 0,
    kAutoTrack = 1,
    kFixed = 2,
  };

  AudioCapture(const AudioConfig& config, std::shared_ptr<AudioRingBuffer> ring);
  ~AudioCapture();

  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  Status Start();
  void Stop();

  bool running() const { return running_.load(); }

 private:
  struct CallbackContext {
    AudioRingBuffer* ring = nullptr;

    int requested_output_channels = 1;
    int device_input_channels = 1;
    int sample_rate_hz = 16000;
    int frames_per_buffer = 320;
    int min_callbacks_between_switches = 8;
    int track_switch_consecutive = 3;
    int fixed_channel_index = 0;
    ChannelSelectMode channel_select_mode = ChannelSelectMode::kAutoTrack;

    std::atomic<std::uint64_t> callback_count{0};
    std::atomic<int> selected_channel{0};
    std::atomic<std::uint64_t> callbacks_since_switch{0};
    bool auto_once_locked = false;
    int pending_better_channel = -1;
    int pending_better_count = 0;

    std::vector<double> rms_accumulator;
    std::vector<double> rms_ema;
    std::vector<float> peak_accumulator;
    std::vector<float> mono_buffer;
  };

  static int PaCallback(const void* input,
                        void* output,
                        unsigned long frame_count,
                        const PaStreamCallbackTimeInfo* time_info,
                        PaStreamCallbackFlags status_flags,
                        void* user_data);

  Status InitializePortAudioIfNeeded();
  Status OpenInputStream();
  int ResolveInputDeviceIndex() const;
  void PrintResolvedDevices() const;

  AudioConfig config_;
  std::shared_ptr<AudioRingBuffer> ring_;
  std::atomic<bool> running_{false};

  PaStream* stream_ = nullptr;
  CallbackContext callback_context_{};
  bool pa_initialized_ = false;

  int opened_input_device_index_ = -1;
  int opened_device_input_channels_ = 1;
};

}  // namespace mos::vis
